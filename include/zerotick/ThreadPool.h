#pragma once

#include "Worker.h"
#include "Risk.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

namespace zerotick {

constexpr std::size_t kWorkerCount = 20;
constexpr std::size_t kActiveWorkerCount = kWorkerCount;

/**
 * Fixed-size thread pool where one thread is permanently associated with one
 * of the top-20 symbols.
 *
 * This is not a general task queue: no symbol is rescheduled onto another
 * thread, avoiding state migration and ensuring each worker owns one position.
 */
class ThreadPool {
public:
    explicit ThreadPool(
        KiteApi& api,
        const std::vector<std::string>& symbols,
        std::chrono::milliseconds pollInterval,
        bool liveTrading,
        double totalAvailableCash);

    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void start();
    void stop();

private:
    KiteApi& api_;
    std::vector<std::string> symbols_;
    std::chrono::milliseconds pollInterval_;
    bool liveTrading_;
    PortfolioRiskManager portfolioRisk_;
    std::atomic_bool stopRequested_{false};
    std::vector<std::thread> threads_;
};

} // namespace zerotick
