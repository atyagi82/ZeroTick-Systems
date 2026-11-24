#pragma once

#include "EngineTypes.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace zerotick {

class StructuredEventWriter {
public:
    explicit StructuredEventWriter(const std::string& path,
                                   std::uint64_t maxFileBytes = 0);

    void recordSession(const std::string& mode);
    void recordSnapshot(const MarketSnapshot& snapshot);
    std::string recordSelection(const MarketSnapshot& snapshot,
                                const StockCandidate& candidate,
                                bool accepted,
                                const std::string& reason);
    std::string recordDecision(const StockCandidate& candidate,
                               const MarketSnapshot& snapshot,
                               bool accepted,
                               const TradeSignal& signal,
                               const std::string& reason);
    void recordTradeEvent(const TradeSignal& signal,
                          const std::string& eventType,
                          int quantity,
                          double price,
                          const std::string& reason,
                          const MarketSnapshot* snapshot = nullptr);
    std::string statusLine() const;

private:
    std::string nextId(const char* prefix);
    std::string partPath(std::uint64_t part) const;
    void openInitialPart();
    void openPart(std::uint64_t part);
    void writeLine(const std::string& line);

    mutable std::mutex mutex_;
    std::ofstream output_;
    std::string basePath_;
    std::uint64_t maxFileBytes_ = 0;
    std::uint64_t currentBytes_ = 0;
    std::uint64_t currentPart_ = 1;
    std::string sessionId_;
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> snapshots_{0};
    std::atomic<std::uint64_t> selected_{0};
    std::atomic<std::uint64_t> signals_{0};
    std::atomic<std::uint64_t> pendingEntries_{0};
    std::atomic<std::uint64_t> openPositions_{0};
    std::atomic<std::uint64_t> completedTrades_{0};
    std::atomic<std::uint64_t> winningTrades_{0};
    mutable std::mutex stateMutex_;
    std::unordered_map<std::string, std::pair<double, int>> openEntries_;
    double realizedProfitLoss_ = 0.0;
};

} // namespace zerotick
