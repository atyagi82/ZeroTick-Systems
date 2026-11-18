#include "zerotick/ThreadPool.h"
#include "zerotick/Strategy.h"

#include <iostream>
#include <stdexcept>

namespace zerotick {

ThreadPool::ThreadPool(
    KiteApi& api,
    const std::vector<std::string>& symbols,
    std::chrono::milliseconds pollInterval)
    : api_(api),
      symbols_(symbols),
      pollInterval_(pollInterval) {
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
    threads_.reserve(kWorkerCount);

    for (const std::string& symbol : symbols_) {
        // Each thread owns one independent worker and never migrates symbols.
        threads_.emplace_back([this, symbol] {
            Worker worker(
                symbol,
                api_,
                std::make_unique<CombinedStrategy>(),
                pollInterval_,
                stopRequested_);
            worker.run();
        });
    }

    std::cout << "Started " << kWorkerCount
              << " stock worker threads\n";
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
