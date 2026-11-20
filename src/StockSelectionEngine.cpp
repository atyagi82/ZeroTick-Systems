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
        // Never let delayed queue entries update selection state.
        if (std::chrono::steady_clock::now() - snapshot.receivedAt >
            config_.maxQuoteAge) {
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

            // The registry avoids duplicate in-flight evaluations for a symbol.
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
    const double alpha = config_.volumeEwmaAlpha;
    auto existing = averageVolume_.find(symbol);
    double avg = existing == averageVolume_.end() ? 0.0 : existing->second;
    const double current = static_cast<double>(volume);
    // The snapshots carry cumulative session volume; smooth successive samples
    // rather than treating the value as an independent per-period volume.
    if (avg <= 0.0) {
        avg = current;
    } else {
        avg = alpha * current + (1.0 - alpha) * avg;
    }
    averageVolume_[symbol] = avg;
    return avg;
}

double StockSelectionEngine::priceChangePercent(
    const MarketSnapshot& snapshot) const {
    if (!std::isfinite(snapshot.lastPrice) || snapshot.lastPrice <= 0.0) {
        return 0.0;
    }

    std::lock_guard<std::mutex> lock(priceMutex_);
    auto existing = previousPrice_.find(snapshot.symbol);
    if (existing == previousPrice_.end()) {
        // The first valid quote establishes the baseline; it cannot establish
        // positive momentum on its own.
        PriceObservation observation;
        observation.price = snapshot.lastPrice;
        observation.receivedAt = snapshot.receivedAt;
        previousPrice_.emplace(snapshot.symbol, observation);
        return 0.0;
    }

    // Multiple selection workers can dequeue successive snapshots concurrently.
    // Do not let a delayed older snapshot roll a symbol's baseline backward.
    if (snapshot.receivedAt <= existing->second.receivedAt) {
        return 0.0;
    }

    const double previous = existing->second.price;
    const double change =
        previous > 0.0
            ? (snapshot.lastPrice - previous) / previous * 100.0
            : 0.0;
    existing->second.price = snapshot.lastPrice;
    existing->second.receivedAt = snapshot.receivedAt;
    return change;
}

bool StockSelectionEngine::evaluateSelection(
    const MarketSnapshot& snapshot, StockCandidate& candidate,
    std::string& rejectionReason) const {
    const std::string& symbol = snapshot.symbol;
    // Track every newer valid quote, even if a later liquidity or volume check
    // rejects it, so momentum always compares adjacent quote observations.
    candidate.priceChangePercent = priceChangePercent(snapshot);

    // Reject low-priced instruments before doing the more expensive filters.
    if (snapshot.lastPrice <= config_.minPrice) {
        rejectionReason = "price";
        return false;
    }

    // Keep candidates within the configured, checked-in market universe.
    if (config_.requireNifty100 && !universe_.contains(symbol)) {
        rejectionReason = "outside_universe";
        return false;
    }

    candidate.lastPrice = snapshot.lastPrice;
    candidate.volume = snapshot.volume;
    candidate.inNifty100 = universe_.contains(symbol);

    // Require both meaningful cumulative volume and activity above its EWMA.
    if (snapshot.volume < config_.minAbsoluteVolume) {
        rejectionReason = "absolute_volume";
        return false;
    }
    double avgVolume = updateAverageVolume(symbol, snapshot.volume);
    // The EWMA includes this snapshot, so the first sample has a ratio of 1.0.
    candidate.relativeVolume =
        avgVolume > 0.0 ? static_cast<double>(snapshot.volume) / avgVolume : 1.0;
    if (candidate.relativeVolume < config_.minRelativeVolume) {
        rejectionReason = "relative_volume";
        return false;
    }

    // Express the best bid/ask spread as a percentage of the midpoint, then
    // require displayed depth on both sides to avoid one-sided books.
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

    candidate.side = candidate.priceChangePercent < 0.0
        ? PositionSide::Short
        : PositionSide::Long;
    const bool shortDisabled =
        candidate.side == PositionSide::Short &&
        !config_.allowShortSelling;
    const bool momentumBelowThreshold =
        candidate.side == PositionSide::Short
            ? candidate.priceChangePercent >
                  -config_.minPriceChangePercent
            : candidate.priceChangePercent <
                  config_.minPriceChangePercent;

    // Require directional momentum and an adequate intraday range; reject
    // excessive movement/range as too volatile.
    double rangePercent = 0.0;
    if (snapshot.highPrice > 0.0 && snapshot.lowPrice > 0.0) {
        rangePercent = (snapshot.highPrice - snapshot.lowPrice) /
                       snapshot.lowPrice * 100.0;
        candidate.intradayRangePercent = rangePercent;
    }
    if (shortDisabled) {
        rejectionReason = "short_disabled";
        return false;
    }
    if (momentumBelowThreshold ||
        rangePercent < config_.minIntradayRangePercent ||
        rangePercent > config_.maxPriceChangePercent ||
        std::abs(candidate.priceChangePercent) > config_.maxPriceChangePercent) {
        rejectionReason = "momentum_or_range";
        return false;
    }

    return true;
}

} // namespace zerotick

