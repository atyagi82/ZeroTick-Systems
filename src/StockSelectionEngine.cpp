#include "zerotick/StockSelectionEngine.h"
#include "zerotick/Log.h"

#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>

namespace zerotick {

StockSelectionEngine::StockSelectionEngine(
    ThreadSafeQueue<MarketSnapshot>& quoteUpdates,
    StockEngineRegistry& registry,
    ThreadSafeQueue<StockCandidate>& outputQueue,
    const StockUniverse& universe, const SelectionConfig& config)
    : quoteUpdates_(quoteUpdates), registry_(registry),
      outputQueue_(outputQueue), universe_(universe), config_(config) {}

StockSelectionEngine::~StockSelectionEngine() { stop(); }

void StockSelectionEngine::start() {
    if (!threads_.empty()) {
        throw std::logic_error("StockSelectionEngine already started");
    }
    stopRequested_.store(false);
    const std::size_t universeSize = universe_.symbols().size();
    threads_.reserve(config_.workerCount);
    for (std::size_t index = 0; index < config_.workerCount; ++index) {
        threads_.emplace_back([this, index] {
            this->workerLoop(index);
        });
    }
    zerotick::logLine("[selection] started " +
                      std::to_string(config_.workerCount) +
                      " worker threads; universe=" +
                      std::to_string(universeSize) +
                      " min_price=" + std::to_string(config_.minPrice) +
                      " min_abs_volume=" +
                      std::to_string(config_.minAbsoluteVolume) +
                      " min_rel_volume=" +
                      std::to_string(config_.minRelativeVolume) +
                      " max_spread_pct=" +
                      std::to_string(config_.maxSpreadPercent) +
                      " min_depth=" +
                      std::to_string(config_.minDepthQuantity) +
                      " min_range_pct=" +
                      std::to_string(config_.minIntradayRangePercent) +
                      " summary_interval_sec=" +
                      std::to_string(config_.summaryInterval.count()));
}

void StockSelectionEngine::stop() {
    stopRequested_.store(true);
    quoteUpdates_.done();
    for (std::thread& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
}

void StockSelectionEngine::workerLoop(std::size_t workerIndex) {
    std::size_t checked = 0;
    std::size_t skippedOwned = 0;
    std::size_t staleQuotes = 0;
    std::size_t selected = 0;
    std::vector<std::string> selectedSymbols;
    std::map<std::string, std::size_t> rejected;
    auto nextSummary = std::chrono::steady_clock::now() +
                       config_.summaryInterval;

    const auto logSummary = [&] {
        if (checked == 0) {
            return;
        }
        std::ostringstream summary;
        summary << "[selection] scan worker=" << workerIndex
                << " quotes=" << checked
                << " skipped_owned=" << skippedOwned
                << " stale=" << staleQuotes
                << " selected=" << selected;
        if (!selectedSymbols.empty()) {
            summary << " selected_symbols=";
            for (std::size_t index = 0; index < selectedSymbols.size(); ++index) {
                if (index > 0) {
                    summary << ',';
                }
                summary << selectedSymbols[index];
            }
        }
        for (const auto& entry : rejected) {
            summary << " rejected_" << entry.first << '=' << entry.second;
        }
        zerotick::logLine(summary.str());
        checked = 0;
        skippedOwned = 0;
        staleQuotes = 0;
        selected = 0;
        selectedSymbols.clear();
        rejected.clear();
        nextSummary = std::chrono::steady_clock::now() +
                      config_.summaryInterval;
    };

    MarketSnapshot snapshot;
    while (!stopRequested_.load() && quoteUpdates_.waitPop(snapshot)) {
        if (stopRequested_.load()) {
            break;
        }
        ++checked;
        const std::string& symbol = snapshot.symbol;
        if (std::chrono::steady_clock::now() - snapshot.receivedAt >
            std::chrono::seconds(5)) {
            ++staleQuotes;
            bool firstStale = false;
            {
                std::lock_guard<std::mutex> lock(staleQuoteMutex_);
                firstStale = staleQuoteSymbols_.insert(symbol).second;
            }
            if (firstStale) {
                zerotick::logLine("[selection] symbol=" + symbol +
                                  " quote_stale");
            }
        } else {
            {
                std::lock_guard<std::mutex> lock(staleQuoteMutex_);
                staleQuoteSymbols_.erase(symbol);
            }

            if (!registry_.claim(symbol, EngineId::Unassigned,
                                 EngineId::Selection)) {
                ++skippedOwned;
            } else {
                StockCandidate candidate;
                candidate.symbol = symbol;
                candidate.selectedAt = std::chrono::steady_clock::now();
                std::string rejectionReason;
                if (evaluateSelection(snapshot, candidate, rejectionReason)) {
                    ++selected;
                    selectedSymbols.push_back(symbol);
                    // Keep Selection ownership until the decision engine claims
                    // it off the output queue.
                    outputQueue_.push(candidate);
                } else {
                    ++rejected[rejectionReason];
                    registry_.release(symbol, EngineId::Selection);
                }
            }
        }

        if (std::chrono::steady_clock::now() >= nextSummary) {
            logSummary();
        }
    }
    logSummary();
}

double StockSelectionEngine::updateAverageVolume(
    const std::string& symbol, std::uint64_t volume) const {
    std::lock_guard<std::mutex> lock(volumeMutex_);
    constexpr double alpha = 0.2;
    auto existing = averageVolume_.find(symbol);
    double avg = existing == averageVolume_.end() ? 0.0 : existing->second;
    const double current = static_cast<double>(volume);
    if (avg <= 0.0) {
        avg = current;
    } else {
        avg = alpha * current + (1.0 - alpha) * avg;
    }
    averageVolume_[symbol] = avg;
    return avg;
}

double StockSelectionEngine::priceChangePercent(
    const std::string& symbol, double lastPrice) const {
    std::lock_guard<std::mutex> lock(priceMutex_);
    auto existing = previousPrice_.find(symbol);
    double previous = existing == previousPrice_.end() ? 0.0 : existing->second;
    double change = 0.0;
    if (previous > 0.0 && lastPrice > 0.0) {
        change = (lastPrice - previous) / previous * 100.0;
    }
    previous = lastPrice > 0.0 ? lastPrice : previous;
    return change;
}

bool StockSelectionEngine::evaluateSelection(
    const MarketSnapshot& snapshot, StockCandidate& candidate,
    std::string& rejectionReason) const {
    const std::string& symbol = snapshot.symbol;

    // 1. Price above 100.
    if (snapshot.lastPrice <= config_.minPrice) {
        rejectionReason = "price";
        return false;
    }

    // 2. Must be in the NIFTY 100 universe.
    if (config_.requireNifty100 && !universe_.contains(symbol)) {
        rejectionReason = "outside_universe";
        return false;
    }

    candidate.lastPrice = snapshot.lastPrice;
    candidate.volume = snapshot.volume;
    candidate.inNifty100 = universe_.contains(symbol);

    // 3. Intelligent volume parameters: absolute + relative (vs recent avg).
    if (snapshot.volume < config_.minAbsoluteVolume) {
        rejectionReason = "absolute_volume";
        return false;
    }
    double avgVolume = updateAverageVolume(symbol, snapshot.volume);
    candidate.relativeVolume =
        avgVolume > 0.0 ? static_cast<double>(snapshot.volume) / avgVolume : 1.0;
    if (candidate.relativeVolume < config_.minRelativeVolume) {
        rejectionReason = "relative_volume";
        return false;
    }

    // 4. Liquidity: spread and book depth.
    double mid = (snapshot.bidPrice + snapshot.askPrice) / 2.0;
    if (mid > 0.0 && snapshot.askPrice >= snapshot.bidPrice &&
        snapshot.bidPrice > 0.0) {
        candidate.spreadPercent =
            (snapshot.askPrice - snapshot.bidPrice) / mid * 100.0;
    } else {
        candidate.spreadPercent = 0.0;
    }
    candidate.bidQuantity = static_cast<double>(snapshot.bidQuantity);
    candidate.askQuantity = static_cast<double>(snapshot.askQuantity);
    if (candidate.spreadPercent > config_.maxSpreadPercent ||
        snapshot.bidQuantity < config_.minDepthQuantity ||
        snapshot.askQuantity < config_.minDepthQuantity) {
        rejectionReason = "liquidity";
        return false;
    }

    // 5. Volatility / momentum: enough intraday range, not excessive.
    candidate.priceChangePercent = priceChangePercent(symbol, snapshot.lastPrice);
    double rangePercent = 0.0;
    if (snapshot.highPrice > 0.0 && snapshot.lowPrice > 0.0) {
        rangePercent = (snapshot.highPrice - snapshot.lowPrice) /
                       snapshot.lowPrice * 100.0;
        candidate.intradayRangePercent = rangePercent;
    }
    if (rangePercent < config_.minIntradayRangePercent ||
        rangePercent > config_.maxPriceChangePercent ||
        std::abs(candidate.priceChangePercent) > config_.maxPriceChangePercent) {
        rejectionReason = "range_or_volatility";
        return false;
    }

    return true;
}

} // namespace zerotick

