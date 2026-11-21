#include "zerotick/StockSelectionEngine.h"
#include "zerotick/Log.h"
#include "zerotick/StructuredEventWriter.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>

namespace zerotick {

StockSelectionEngine::StockSelectionEngine(
    ThreadSafeQueue<MarketSnapshot>& quoteUpdates,
    StockEngineRegistry& registry,
    ThreadSafeQueue<StockCandidate>& outputQueue,
    const StockUniverse& universe, const SelectionConfig& config,
    StructuredEventWriter* eventWriter)
    : quoteUpdates_(quoteUpdates), registry_(registry),
      outputQueue_(outputQueue), universe_(universe), config_(config),
      eventWriter_(eventWriter) {}

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
                    if (eventWriter_) {
                        candidate.candidateId = eventWriter_->recordSelection(
                            snapshot, candidate, true, "passed");
                    }
                    ++selected;
                    selectedSymbols.push_back(symbol);
                    // Keep Selection ownership until the decision engine claims
                    // it off the output queue.
                    outputQueue_.push(candidate);
                } else {
                    if (eventWriter_) {
                        eventWriter_->recordSelection(
                            snapshot, candidate, false, rejectionReason);
                    }
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

double StockSelectionEngine::updateRelativeVolume(
    const std::string& symbol, std::uint64_t cumulativeVolume) const {
    std::lock_guard<std::mutex> lock(volumeMutex_);
    const auto previous = previousVolume_.find(symbol);
    if (previous == previousVolume_.end() ||
        cumulativeVolume < previous->second) {
        previousVolume_[symbol] = cumulativeVolume;
        averageVolume_.erase(symbol);
        return 1.0;
    }
    const std::uint64_t intervalVolume =
        cumulativeVolume - previous->second;
    previousVolume_[symbol] = cumulativeVolume;

    const double alpha = config_.volumeEwmaAlpha;
    auto existing = averageVolume_.find(symbol);
    double avg = existing == averageVolume_.end() ? 0.0 : existing->second;
    const double current = static_cast<double>(intervalVolume);
    if (avg <= 0.0) {
        avg = current;
    } else {
        avg = alpha * current + (1.0 - alpha) * avg;
    }
    averageVolume_[symbol] = avg;
    return avg > 0.0 ? current / avg : 0.0;
}

bool StockSelectionEngine::priceChanges(
    const MarketSnapshot& snapshot, double& momentumChangePercent,
    double& trendChangePercent) const {
    momentumChangePercent = 0.0;
    trendChangePercent = 0.0;
    if (!std::isfinite(snapshot.lastPrice) || snapshot.lastPrice <= 0.0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(priceMutex_);
    std::deque<PriceObservation>& history = priceHistory_[snapshot.symbol];
    if (!history.empty() &&
        snapshot.receivedAt <= history.back().receivedAt) {
        return false;
    }
    if (history.empty()) {
        PriceObservation observation;
        observation.price = snapshot.lastPrice;
        observation.receivedAt = snapshot.receivedAt;
        history.push_back(observation);
        return false;
    }

    const auto referenceFor = [&](std::size_t lookbackSeconds,
                                  double& referencePrice) {
        if (lookbackSeconds == 0) {
            referencePrice = history.back().price;
            return true;
        }
        const auto target = snapshot.receivedAt -
            std::chrono::seconds(lookbackSeconds);
        for (auto observation = history.rbegin();
             observation != history.rend(); ++observation) {
            if (observation->receivedAt <= target) {
                referencePrice = observation->price;
                return true;
            }
        }
        return false;
    };

    double momentumReference = 0.0;
    double trendReference = 0.0;
    const bool momentumReady = referenceFor(
        config_.momentumLookbackSeconds, momentumReference);
    const bool trendReady = config_.trendLookbackSeconds == 0 ||
        referenceFor(config_.trendLookbackSeconds, trendReference);
    if (momentumReady && momentumReference > 0.0) {
        momentumChangePercent =
            (snapshot.lastPrice - momentumReference) /
            momentumReference * 100.0;
    }
    if (config_.trendLookbackSeconds == 0) {
        trendChangePercent = momentumChangePercent;
    } else if (trendReady && trendReference > 0.0) {
        trendChangePercent =
            (snapshot.lastPrice - trendReference) /
            trendReference * 100.0;
    }

    PriceObservation observation;
    observation.price = snapshot.lastPrice;
    observation.receivedAt = snapshot.receivedAt;
    history.push_back(observation);
    const std::size_t longestLookback = std::max(
        config_.momentumLookbackSeconds, config_.trendLookbackSeconds);
    const auto oldestNeeded = snapshot.receivedAt -
        std::chrono::seconds(longestLookback + 5);
    while (history.size() > 2 &&
           history.front().receivedAt < oldestNeeded) {
        history.pop_front();
    }
    return momentumReady && trendReady;
}

bool StockSelectionEngine::evaluateSelection(
    const MarketSnapshot& snapshot, StockCandidate& candidate,
    std::string& rejectionReason) const {
    const std::string& symbol = snapshot.symbol;
    // Track every newer valid quote, even if a later liquidity or volume check
    // rejects it, so momentum always compares adjacent quote observations.
    const bool momentumReady = priceChanges(
        snapshot, candidate.priceChangePercent,
        candidate.trendChangePercent);

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
    candidate.relativeVolume =
        updateRelativeVolume(symbol, snapshot.volume);
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
    const bool trendBelowThreshold =
        candidate.side == PositionSide::Short
            ? candidate.trendChangePercent >
                  -config_.minTrendChangePercent
            : candidate.trendChangePercent <
                  config_.minTrendChangePercent;

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
    if (!momentumReady || momentumBelowThreshold || trendBelowThreshold ||
        rangePercent < config_.minIntradayRangePercent ||
        rangePercent > config_.maxPriceChangePercent ||
        std::abs(candidate.priceChangePercent) > config_.maxPriceChangePercent) {
        rejectionReason = "momentum_or_range";
        return false;
    }

    return true;
}

} // namespace zerotick

