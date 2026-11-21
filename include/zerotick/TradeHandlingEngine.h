#pragma once

#include "EngineTypes.h"
#include "KiteApi.h"
#include "Risk.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zerotick {

class StructuredEventWriter;

/**
 * TradeHandlingEngine
 * -------------------
 * Consumes TradeSignals from the TradingDecisionEngine and performs the actual
 * trade execution: size the position, place the stop-limit entry, monitor the
 * fill, place the target/stop-loss exit, and release the symbol when the round
 * trip completes.
 *
 * Multiple worker threads pull signals off the shared input queue. Ownership
 * transfers Decision -> Handling when a signal is popped, and is released back
 * to Unassigned once the position is closed or the attempt fails -- a symbol is
 * never in two engines at once.
 */
class TradeHandlingEngine {
public:
    TradeHandlingEngine(KiteApi& api, StockEngineRegistry& registry,
                        ThreadSafeQueue<TradeSignal>& inputQueue,
                        PortfolioRiskManager& portfolioRisk,
                        const HandlingConfig& config,
                        StructuredEventWriter* eventWriter = nullptr);
    ~TradeHandlingEngine();

    TradeHandlingEngine(const TradeHandlingEngine&) = delete;
    TradeHandlingEngine& operator=(const TradeHandlingEngine&) = delete;

    void start();
    void stop();

private:
    void workerLoop();
    void handleSignal(const TradeSignal& signal);
    bool waitForEntryFill(const std::string& orderId,
                          BrokerOrderStatus& status) const;
    void manageExit(const TradeSignal& signal, int quantity,
                    double entryPrice);
    bool waitForShadowEntry(const TradeSignal& signal, int quantity,
                            double& entryPrice);
    void manageShadowExit(const TradeSignal& signal, int quantity,
                          double entryPrice);
    bool waitForExitFill(const std::string& orderId,
                         BrokerOrderStatus& status) const;
    double roundUpToTick(double price, double tick) const;

    KiteApi& api_;
    StockEngineRegistry& registry_;
    ThreadSafeQueue<TradeSignal>& inputQueue_;
    PortfolioRiskManager& portfolioRisk_;
    HandlingConfig config_;
    StructuredEventWriter* eventWriter_ = nullptr;
    std::atomic_bool stopRequested_{false};
    std::vector<std::thread> threads_;
};

} // namespace zerotick
