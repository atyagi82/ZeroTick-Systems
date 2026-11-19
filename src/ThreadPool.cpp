#include "zerotick/ThreadPool.h"
#include "zerotick/Log.h"
#include "zerotick/Strategy.h"

#include <stdexcept>

namespace zerotick {

ThreadPool::ThreadPool(
    KiteApi& api,
    const std::vector<std::string>& symbols,
    std::chrono::milliseconds pollInterval,
    bool liveTrading)
    : api_(api),
      symbols_(symbols),
      pollInterval_(pollInterval),
      liveTrading_(liveTrading) {
    if (symbols_.size() != kWorkerCount) {
        throw std::invalid_argument(
            "ThreadPool requires exactly 20 symbols");
    }
}

ThreadPool::~ThreadPool() {
    stop();
}

void ThreadPool::start() {
    if (!threads_.empty()) {
        throw std::logic_error("ThreadPool has already been started");
    }

    stopRequested_.store(false);
    threads_.reserve(kActiveWorkerCount);

    // Keep the 20-symbol quote universe loaded, but only run the first
    // configured stock worker during the current live-data validation.
    for (std::size_t index = 0; index < kActiveWorkerCount; ++index) {
        const std::string symbol = symbols_[index];
        threads_.emplace_back([this, symbol] {
            Worker worker(
                symbol,
                api_,
                std::make_unique<OrderImbalanceStrategy>(),
                pollInterval_,
                stopRequested_,
                liveTrading_);
            worker.run();
        });
    }

    logLine(
        "Started " + std::to_string(kActiveWorkerCount) + " of " +
        std::to_string(kWorkerCount) + " stock worker threads; " +
        std::to_string(kWorkerCount - kActiveWorkerCount) +
        " disabled; active_symbol=" + symbols_.front() +
        " live_trading=" + (liveTrading_ ? "enabled" : "disabled"));
}

void ThreadPool::stop() {
    stopRequested_.store(true);

    for (std::thread& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    threads_.clear();
}

} // namespace zerotick
