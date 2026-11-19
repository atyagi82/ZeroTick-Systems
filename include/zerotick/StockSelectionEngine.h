#pragma once

#include "EngineTypes.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zerotick {

/**
 * StockSelectionEngine
 * --------------------
 * Selects stocks for intraday trade from the configured universe (NIFTY 100)
 * using liquidity/volatility/volume parameters, then
 * publishes qualifying StockCandidates to the TradingDecisionEngine.
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
                         const SelectionConfig& config);
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
    double updateAverageVolume(const std::string& symbol,
                               std::uint64_t volume) const;
    double priceChangePercent(const std::string& symbol,
                              double lastPrice) const;

    ThreadSafeQueue<MarketSnapshot>& quoteUpdates_;
    StockEngineRegistry& registry_;
    ThreadSafeQueue<StockCandidate>& outputQueue_;
    const StockUniverse& universe_;
    SelectionConfig config_;
    std::atomic_bool stopRequested_{false};
    std::vector<std::thread> threads_;
    mutable std::mutex staleQuoteMutex_;
    std::unordered_set<std::string> staleQuoteSymbols_;
    mutable std::mutex volumeMutex_;
    mutable std::unordered_map<std::string, double> averageVolume_;
    mutable std::mutex priceMutex_;
    mutable std::unordered_map<std::string, double> previousPrice_;
};

} // namespace zerotick
