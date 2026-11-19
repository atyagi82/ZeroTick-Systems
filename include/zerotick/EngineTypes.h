#pragma once

#include "Types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zerotick {

// Identifies which engine currently owns a stock symbol. A symbol may be owned
// by at most one engine at any point in time.
enum class EngineId {
    Unassigned = 0,
    Selection = 1,
    Decision = 2,
    Handling = 3
};

inline const char* engineIdToString(EngineId id) {
    switch (id) {
        case EngineId::Unassigned: return "Unassigned";
        case EngineId::Selection: return "StockSelection";
        case EngineId::Decision: return "TradingDecision";
        case EngineId::Handling: return "TradeHandling";
    }
    return "Unknown";
}

/** A stock that passed selection and is offered to the trading decision engine. */
struct StockCandidate {
    std::string symbol;
    double lastPrice = 0.0;
    std::uint64_t volume = 0;
    double relativeVolume = 0.0;
    double priceChangePercent = 0.0;
    double spreadPercent = 0.0;
    double bidQuantity = 0.0;
    double askQuantity = 0.0;
    double intradayRangePercent = 0.0;
    bool inNifty100 = false;
    std::chrono::steady_clock::time_point selectedAt;
};

/** A trade decision handed to the trade handling engine for execution. */
struct TradeSignal {
    std::string symbol;
    int quantity = 0;
    double entryTriggerPrice = 0.0;
    double entryLimitPrice = 0.0;
    double targetPrice = 0.0;
    double stopLossPrice = 0.0;
    std::chrono::steady_clock::time_point createdAt;
};

/** Result of a handled trade, emitted for observability. */
struct TradeOutcome {
    std::string symbol;
    bool entryFilled = false;
    double filledEntryPrice = 0.0;
    int filledQuantity = 0;
    bool exited = false;
    double exitPrice = 0.0;
    bool stopLoss = false;
    std::string reason;
};

/** Configuration for the StockSelectionEngine. */
struct SelectionConfig {
    double minPrice = 100.0;                  // stocks above 100
    bool requireNifty100 = true;              // in NIFTY 100 universe
    double minRelativeVolume = 1.5;           // current volume vs recent average
    std::uint64_t minAbsoluteVolume = 50000;  // minimum absolute volume
    double maxSpreadPercent = 0.20;           // liquidity ceiling
    double minDepthQuantity = 1000.0;         // min bid/ask book depth
    double minPriceChangePercent = 0.05;      // some intraday momentum
    double maxPriceChangePercent = 6.0;       // avoid circuits / excess volatility
    double minIntradayRangePercent = 0.30;    // enough intraday range to trade
    std::chrono::seconds summaryInterval{5};
    std::size_t workerCount = 4;
    double defaultEntryBudget = 50000.0;      // indicative cash per candidate
};

/** Configuration for the TradingDecisionEngine. */
struct DecisionConfig {
    double minimumImbalance = 0.05;           // weighted bid-side imbalance
    double maximumSpreadPercent = 0.10;
    double targetPercent = 0.20;              // profit target percent
    double stopPercent = 0.40;                // stop loss percent
    std::size_t requiredConfirmations = 1;    // snapshot confirmations
    bool requireVolumeConfirmation = true;
    double minRelativeVolume = 1.2;
    std::chrono::milliseconds pollInterval{500};
    std::size_t workerCount = 4;
};

/** Configuration for the TradeHandlingEngine. */
struct HandlingConfig {
    std::chrono::milliseconds pollInterval{1000};
    std::chrono::seconds entryFillTimeout{30};
    std::chrono::seconds exitFillTimeout{30};
    std::size_t workerCount = 4;
    bool liveTrading = false;
};

/** Source of the tradable universe (e.g. NIFTY 100 constituents). */
class StockUniverse {
public:
    virtual ~StockUniverse() = default;
    virtual std::vector<std::string> symbols() const = 0;
    virtual bool contains(const std::string& symbol) const = 0;
};

/** Default universe using the checked-in NSE NIFTY 100 constituent snapshot. */
class NiftyUniverse : public StockUniverse {
public:
    NiftyUniverse();
    std::vector<std::string> symbols() const override;
    bool contains(const std::string& symbol) const override;

private:
    std::vector<std::string> symbols_;
    mutable std::mutex mutex_;
    std::unordered_set<std::string> set_;
};

/**
 * Thread-safe ownership registry enforcing "one stock, one engine at a time".
 *
 * Ownership transitions as a stock flows through the pipeline:
 *   Unassigned -> Selection -> Decision -> Handling -> Unassigned
 * A claim only succeeds when the symbol is currently owned by `expectedOwner`,
 * so a symbol can never be active in two engines simultaneously.
 */
class StockEngineRegistry {
public:
    // Atomically moves symbol from expectedOwner to newOwner. Returns false if
    // the symbol is not currently owned by expectedOwner.
    bool claim(const std::string& symbol, EngineId expectedOwner,
               EngineId newOwner);
    bool release(const std::string& symbol, EngineId owner);
    EngineId ownerOf(const std::string& symbol) const;
    bool isUnassigned(const std::string& symbol) const;
    std::size_t countInEngine(EngineId engine) const;
    void releaseAll();

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, EngineId> ownership_;
};

/**
 * Bounded/unbounded multi-producer multi-consumer queue with blocking pop.
 * All engines communicate through these queues.
 */
template <typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue() = default;
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    void push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(item));
        cv_.notify_one();
    }

    bool tryPop(T& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty() || done_) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // Blocks until an item is available or the queue is marked done.
    // Returns true if an item was popped, false if done and empty.
    bool waitPop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || done_; });
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    bool pop(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, timeout,
                         [this] { return !queue_.empty() || done_; })) {
            if (queue_.empty()) {
                return false;
            }
            out = std::move(queue_.front());
            queue_.pop();
            return true;
        }
        return false;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // Signals producers are finished; unblocks all waiters (they observe done).
    void done() {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
        cv_.notify_all();
    }

    bool isDone() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return done_;
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
};

} // namespace zerotick

