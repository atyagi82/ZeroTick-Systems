#pragma once

#include "KiteApi.h"
#include "Risk.h"
#include "StateMachine.h"
#include "Strategy.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace zerotick {

/**
 * Owns and monitors exactly one symbol.
 *
 * A Worker never monitors multiple symbols. This makes state and position
 * tracking independent and keeps each pool thread's responsibility explicit.
 */
class Worker {
public:
    Worker(
        std::string symbol,
        KiteApi& api,
        std::unique_ptr<Strategy> strategy,
        std::chrono::milliseconds pollInterval,
        std::atomic_bool& stopRequested,
        bool liveTrading,
        PortfolioRiskManager& portfolioRisk);

    void run();
    WorkerState state() const;
    const std::string& symbol() const noexcept;

private:
    void start();
    void monitor();
    void handleEntry();
    void handleEntryPending();
    void handlePosition();
    void handleExit(const MarketSnapshot& snapshot);
    void handleExitPending();
    void submitExit(const MarketSnapshot& snapshot, bool stopLoss);
    void stop();

    std::string symbol_;
    KiteApi& api_;
    std::unique_ptr<Strategy> strategy_;
    std::chrono::milliseconds pollInterval_;
    std::atomic_bool& stopRequested_;
    bool liveTrading_;
    PortfolioRiskManager& portfolioRisk_;

    mutable std::mutex stateMutex_;
    WorkerStateMachine stateMachine_;
    double entryPrice_ = 0.0;
    int quantity_ = 0;
    std::string entryOrderId_;
    std::string exitOrderId_;
    std::chrono::steady_clock::time_point entryOrderSubmittedAt_;
    std::chrono::steady_clock::time_point exitOrderSubmittedAt_;
    std::chrono::steady_clock::time_point exitRetryAfter_;
    std::chrono::steady_clock::time_point lastStrategySnapshotAt_;
    int exitOrderQuantity_ = 0;
    bool entryCancelRequested_ = false;
    bool exitCancelRequested_ = false;
    bool exitIsStopLoss_ = false;
};

} // namespace zerotick
