#include "zerotick/StructuredEventWriter.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace zerotick {
namespace {

using Json = nlohmann::json;

std::string utcNow() {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream value;
    value << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
          << std::setw(3) << std::setfill('0') << milliseconds.count() << 'Z';
    return value.str();
}

const char* sideName(PositionSide side) {
    return side == PositionSide::Short ? "SHORT" : "LONG";
}

Json bookLevels(const std::array<OrderBookLevel, 5>& levels) {
    Json result = Json::array();
    for (const OrderBookLevel& level : levels) {
        result.push_back({{"price", level.price}, {"quantity", level.quantity}});
    }
    return result;
}

Json snapshotFields(const MarketSnapshot& snapshot) {
    return {
        {"symbol", snapshot.symbol}, {"last_price", snapshot.lastPrice},
        {"bid_price", snapshot.bidPrice}, {"ask_price", snapshot.askPrice},
        {"bid_quantity", snapshot.bidQuantity},
        {"ask_quantity", snapshot.askQuantity}, {"volume", snapshot.volume},
        {"open", snapshot.openPrice}, {"high", snapshot.highPrice},
        {"low", snapshot.lowPrice}, {"sequence", snapshot.sequence},
        {"bid_levels", bookLevels(snapshot.bidLevels)},
        {"ask_levels", bookLevels(snapshot.askLevels)}
    };
}

} // namespace

StructuredEventWriter::StructuredEventWriter(const std::string& path)
    : output_(path, std::ios::out | std::ios::app) {
    if (!output_) {
        throw std::runtime_error("Unable to open structured event file: " + path);
    }
    sessionId_ = "session-" + utcNow() + "-" + std::to_string(
        static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
}

std::string StructuredEventWriter::nextId(const char* prefix) {
    return std::string(prefix) + '-' + sessionId_ + '-' +
           std::to_string(++sequence_);
}

void StructuredEventWriter::writeLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ << line << '\n';
    output_.flush();
    if (!output_) {
        throw std::runtime_error("Failed to write structured event data");
    }
}

void StructuredEventWriter::recordSession(const std::string& mode) {
    Json event = {{"schema_version", 1}, {"event_type", "session_started"},
                  {"event_id", nextId("evt")}, {"session_id", sessionId_},
                  {"event_time_utc", utcNow()}, {"mode", mode}};
    writeLine(event.dump());
}

void StructuredEventWriter::recordSnapshot(const MarketSnapshot& snapshot) {
    Json event = {{"schema_version", 1}, {"event_type", "market_snapshot"},
                  {"event_id", nextId("evt")}, {"session_id", sessionId_},
                  {"event_time_utc", utcNow()}, {"market", snapshotFields(snapshot)}};
    ++snapshots_;
    writeLine(event.dump());
}

std::string StructuredEventWriter::recordSelection(
    const MarketSnapshot& snapshot, const StockCandidate& candidate,
    bool accepted, const std::string& reason) {
    const std::string candidateId = accepted ? nextId("candidate") : "";
    Json event = {
        {"schema_version", 1}, {"event_type", "selection_decision"},
        {"event_id", nextId("evt")}, {"session_id", sessionId_},
        {"event_time_utc", utcNow()}, {"candidate_id", candidateId},
        {"accepted", accepted}, {"reason", reason},
        {"market", snapshotFields(snapshot)},
        {"features", {{"side", sideName(candidate.side)},
                      {"relative_volume", candidate.relativeVolume},
                      {"price_change_percent", candidate.priceChangePercent},
                      {"spread_percent", candidate.spreadPercent},
                      {"intraday_range_percent", candidate.intradayRangePercent},
                      {"in_nifty_100", candidate.inNifty100}}}
    };
    if (accepted) ++selected_;
    writeLine(event.dump());
    return candidateId;
}

std::string StructuredEventWriter::recordDecision(
    const StockCandidate& candidate, const MarketSnapshot& snapshot,
    bool accepted, const TradeSignal& signal, const std::string& reason) {
    const std::string signalId = accepted ? nextId("signal") : "";
    Json event = {
        {"schema_version", 1}, {"event_type", "trade_decision"},
        {"event_id", nextId("evt")}, {"session_id", sessionId_},
        {"event_time_utc", utcNow()}, {"candidate_id", candidate.candidateId},
        {"signal_id", signalId}, {"accepted", accepted}, {"reason", reason},
        {"market", snapshotFields(snapshot)},
        {"candidate_features", {{"side", sideName(candidate.side)},
                                {"relative_volume", candidate.relativeVolume},
                                {"price_change_percent", candidate.priceChangePercent},
                                {"spread_percent", candidate.spreadPercent},
                                {"intraday_range_percent", candidate.intradayRangePercent}}}
    };
    if (accepted) {
        event["order_plan"] = {{"quantity", signal.quantity},
                               {"entry_trigger", signal.entryTriggerPrice},
                               {"entry_limit", signal.entryLimitPrice},
                               {"target", signal.targetPrice},
                               {"stop_loss", signal.stopLossPrice}};
        ++signals_;
    }
    writeLine(event.dump());
    return signalId;
}

void StructuredEventWriter::recordTradeEvent(
    const TradeSignal& signal, const std::string& eventType, int quantity,
    double price, const std::string& reason, const MarketSnapshot* snapshot) {
    Json event = {{"schema_version", 1}, {"event_type", eventType},
                  {"event_id", nextId("evt")}, {"session_id", sessionId_},
                  {"event_time_utc", utcNow()}, {"signal_id", signal.signalId},
                  {"candidate_id", signal.candidateId}, {"symbol", signal.symbol},
                  {"side", sideName(signal.side)}, {"quantity", quantity},
                  {"price", price}, {"reason", reason}};
    if (snapshot) event["market"] = snapshotFields(*snapshot);
    if (eventType == "entry_pending") ++pendingEntries_;
    if (eventType == "entry_filled") {
        if (pendingEntries_ > 0) --pendingEntries_;
        ++openPositions_;
        std::lock_guard<std::mutex> lock(stateMutex_);
        openEntries_[signal.signalId] = std::make_pair(price, quantity);
    }
    if (eventType == "entry_expired" || eventType == "entry_rejected") {
        if (pendingEntries_ > 0) --pendingEntries_;
    }
    if (eventType == "position_closed") {
        if (openPositions_ > 0) --openPositions_;
        ++completedTrades_;
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto entry = openEntries_.find(signal.signalId);
        if (entry != openEntries_.end()) {
            const double change = signal.side == PositionSide::Long
                ? price - entry->second.first
                : entry->second.first - price;
            const double profitLoss = change * entry->second.second;
            realizedProfitLoss_ += profitLoss;
            if (profitLoss > 0.0) ++winningTrades_;
            event["outcome"] = {{"entry_price", entry->second.first},
                                {"exit_price", price},
                                {"gross_profit_loss", profitLoss}};
            openEntries_.erase(entry);
        }
    }
    writeLine(event.dump());
}

std::string StructuredEventWriter::statusLine() const {
    double realizedProfitLoss = 0.0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        realizedProfitLoss = realizedProfitLoss_;
    }
    std::ostringstream status;
    status << "snapshots=" << snapshots_.load()
           << " selected=" << selected_.load()
           << " signals=" << signals_.load()
           << " pending=" << pendingEntries_.load()
           << " open=" << openPositions_.load()
           << " completed=" << completedTrades_.load()
           << " wins=" << winningTrades_.load()
           << " gross_pnl=" << std::fixed << std::setprecision(2)
           << realizedProfitLoss;
    return status.str();
}

} // namespace zerotick
