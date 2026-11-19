#pragma once

#include "EngineTypes.h"
#include "KiteApi.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace zerotick {

/**
 * TradingDecisionEngine
 * ---------------------
 * Consumes StockCandidates from the StockSelectionEngine and decides whether
 * it is good to intraday trade each stock right now. Qualifying decisions are
 * published as TradeSignals to the TradeHandlingEngine.
 *
 * Multiple worker threads pull candidates off the shared input queue. Ownership
 * transfers Selection -> Decision when a candidate is popped, and Decision ->
 * Handling when a signal is pushed -- a symbol is never in two engines at once.
 */
class TradingDecisionEngine {
public:
    TradingDecisionEngine(KiteApi& api, StockEngineRegistry& registry,
                          ThreadSafeQueue<StockCandidate>& inputQueue,
                          ThreadSafeQueue<TradeSignal>& outputQueue,
                          const DecisionConfig& config);
    ~TradingDecisionEngine();

    TradingDecisionEngine(const TradingDecisionEngine&) = delete;
    TradingDecisionEngine& operator=(const TradingDecisionEngine&) = delete;

    void start();
    void stop();

private:
    void workerLoop();
    bool evaluateDecision(const StockCandidate& candidate,
                          const MarketSnapshot& fresh,
                          TradeSignal& signal) const;
    double weightedImbalance(const MarketSnapshot& snapshot) const;

    KiteApi& api_;
    StockEngineRegistry& registry_;
    ThreadSafeQueue<StockCandidate>& inputQueue_;
    ThreadSafeQueue<TradeSignal>& outputQueue_;
    DecisionConfig config_;
    std::atomic_bool stopRequested_{false};
    std::vector<std::thread> threads_;
};

} // namespace zerotick
