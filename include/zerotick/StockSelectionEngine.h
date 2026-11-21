#pragma once

#include "EngineTypes.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zerotick {

class StructuredEventWriter;

/**
 * StockSelectionEngine
 * --------------------
 * Selects stocks for intraday trade from the configured universe using
 * price-movement, liquidity, volatility, and volume filters, then publishes
 * qualifying StockCandidates to the TradingDecisionEngine.
 *
 * Runs multiple workers that wait for quote updates. A symbol is claimed into
 * the Selection engine for the duration of its evaluation and, once selected,
 * stays owned by Selection until the decision engine claims it off the output
 * queue -- enforcing "one stock, one engine at a time".
 */
class StockSelectionEngine {
public:
    StockSelectionEngine(ThreadSafeQueue<MarketSnapshot>& quoteUpdates,
                         StockEngineRegistry& registry,
                         ThreadSafeQueue<StockCandidate>& outputQueue,
                         const StockUniverse& universe,
                         const SelectionConfig& config,
                         StructuredEventWriter* eventWriter = nullptr);
    ~StockSelectionEngine();

    StockSelectionEngine(const StockSelectionEngine&) = delete;
    StockSelectionEngine& operator=(const StockSelectionEngine&) = delete;

    void start();
    void stop();

private:
    void workerLoop(std::size_t workerIndex);
    bool evaluateSelection(const MarketSnapshot& snapshot,
                           StockCandidate& candidate,
                           std::string& rejectionReason) const;
    double updateRelativeVolume(const std::string& symbol,
                                std::uint64_t cumulativeVolume) const;

    // The receipt time lets parallel workers reject delayed events without
    // replacing a symbol's baseline with an older quote.
    struct PriceObservation {
        double price = 0.0;
        std::chrono::steady_clock::time_point receivedAt;
    };

    bool priceChanges(const MarketSnapshot& snapshot,
                      double& momentumChangePercent,
                      double& trendChangePercent) const;

    ThreadSafeQueue<MarketSnapshot>& quoteUpdates_;
    StockEngineRegistry& registry_;
    ThreadSafeQueue<StockCandidate>& outputQueue_;
    const StockUniverse& universe_;
    SelectionConfig config_;
    StructuredEventWriter* eventWriter_ = nullptr;
    std::atomic_bool stopRequested_{false};
    std::vector<std::thread> threads_;
    mutable std::mutex staleQuoteMutex_;
    std::unordered_set<std::string> staleQuoteSymbols_;
    mutable std::mutex volumeMutex_;
    mutable std::unordered_map<std::string, double> averageVolume_;
    mutable std::unordered_map<std::string, std::uint64_t> previousVolume_;
    mutable std::mutex priceMutex_;
    mutable std::unordered_map<std::string,
                               std::deque<PriceObservation>> priceHistory_;
};

} // namespace zerotick
