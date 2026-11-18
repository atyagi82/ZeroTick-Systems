#pragma once

#include "KiteApi.h"
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
        std::atomic_bool& stopRequested);

    void run();
    WorkerState state() const;
    const std::string& symbol() const noexcept;

private:
    void start();
    void monitor();
    void handleEntry();
    void handlePosition();
    void handleExit();
    void stop();

    std::string symbol_;
    KiteApi& api_;
    std::unique_ptr<Strategy> strategy_;
    std::chrono::milliseconds pollInterval_;
    std::atomic_bool& stopRequested_;

    mutable std::mutex stateMutex_;
    WorkerStateMachine stateMachine_;
    double entryPrice_ = 0.0;
    int quantity_ = 1;
};

} // namespace zerotick
