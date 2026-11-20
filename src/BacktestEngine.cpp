#include "zerotick/BacktestEngine.h"
#include "zerotick/Log.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace zerotick {
namespace {

struct BarEvent {
    std::string symbol;
    HistoricalCandle candle;
};

struct SymbolState {
    std::uint64_t cumulativeVolume = 0;
    double averageVolume = 0.0;
    double previousClose = 0.0;
    std::string previousTimestamp;
    double sessionHigh = 0.0;
    double sessionLow = 0.0;
    double latestClose = 0.0;
    std::string latestTimestamp;
};

struct PendingEntry {
    BacktestTrade trade;
    double reservedBudget = 0.0;
    std::string signalDate;
    int signalMinute = 0;
};

struct OpenPosition {
    BacktestTrade trade;
};

double roundUpToTick(double price, double tickSize) {
    return std::ceil(price / tickSize - 1e-9) * tickSize;
}

double roundDownToTick(double price, double tickSize) {
    return std::floor(price / tickSize + 1e-9) * tickSize;
}

int parseClockTime(const std::string& value, const std::string& description) {
    if (value.size() != 5 || value[2] != ':' ||
        value[0] < '0' || value[0] > '9' ||
        value[1] < '0' || value[1] > '9' ||
        value[3] < '0' || value[3] > '9' ||
        value[4] < '0' || value[4] > '9') {
        throw std::invalid_argument(
            description + " must use HH:MM format");
    }

    const int hour = (value[0] - '0') * 10 + value[1] - '0';
    const int minute = (value[3] - '0') * 10 + value[4] - '0';
    if (hour > 23 || minute > 59) {
        throw std::invalid_argument(
            description + " is outside the valid clock range");
    }
    return hour * 60 + minute;
}

int candleMinuteOfDay(const std::string& timestamp) {
    if (timestamp.size() < 16 || timestamp[10] != 'T' ||
        timestamp[13] != ':') {
        throw std::invalid_argument(
            "Historical candle timestamp must include ISO HH:MM time");
    }
    return parseClockTime(timestamp.substr(11, 5),
                          "Historical candle time");
}

void validateCandle(const std::string& symbol,
                    const HistoricalCandle& candle) {
    if (candle.timestamp.empty() ||
        !std::isfinite(candle.open) || !std::isfinite(candle.high) ||
        !std::isfinite(candle.low) || !std::isfinite(candle.close) ||
        candle.open <= 0.0 || candle.high <= 0.0 ||
        candle.low <= 0.0 || candle.close <= 0.0 ||
        candle.high < candle.low || candle.high < candle.open ||
        candle.high < candle.close || candle.low > candle.open ||
        candle.low > candle.close) {
        throw std::invalid_argument(
            "Invalid historical OHLCV candle for " + symbol);
    }
}

void closePosition(OpenPosition& position,
                   const std::string& exitTime,
                   double exitPrice,
                   const std::string& reason,
                   double& cash,
                   BacktestResult& result) {
    BacktestTrade trade = position.trade;
    trade.exitTime = exitTime;
    trade.exitPrice = exitPrice;
    trade.exitReason = reason;
    const double positionChange =
        trade.side == PositionSide::Long
            ? trade.exitPrice - trade.entryPrice
            : trade.entryPrice - trade.exitPrice;
    trade.grossProfitLoss = positionChange * trade.quantity;
    logLine(
        "[backtest] position_closed symbol=" + trade.symbol +
        " side=" +
        (trade.side == PositionSide::Short ? "SHORT" : "LONG") +
        " entry=" + std::to_string(trade.entryPrice) +
        " exit=" + std::to_string(trade.exitPrice) +
        " reason=" + reason +
        " gross_pnl=" + std::to_string(trade.grossProfitLoss));
    if (trade.side == PositionSide::Long) {
        cash += trade.exitPrice * trade.quantity;
    } else {
        cash -= trade.exitPrice * trade.quantity;
    }
    result.trades.push_back(std::move(trade));
}

double equityAtLatestCloses(
    double cash,
    const std::map<std::string, OpenPosition>& positions,
    const std::map<std::string, SymbolState>& symbolStates) {
    double equity = cash;
    for (const auto& entry : positions) {
        const auto state = symbolStates.find(entry.first);
        if (state != symbolStates.end()) {
            const double marketValue =
                entry.second.trade.quantity * state->second.latestClose;
            equity += entry.second.trade.side == PositionSide::Long
                ? marketValue
                : -marketValue;
        }
    }
    return equity;
}

} // namespace

BacktestResult BacktestEngine::run(
    const StockUniverse& universe,
    const std::map<std::string, std::vector<HistoricalCandle>>& candlesBySymbol,
    const std::map<std::string, double>& tickSizes,
    const BacktestConfig& config) const {
    if (!std::isfinite(config.initialCapital) ||
        config.initialCapital <= 0.0 ||
        config.maxOpenPositions == 0 ||
        !std::isfinite(config.fallbackTickSize) ||
        config.fallbackTickSize <= 0.0 ||
        config.entryTimeoutMinutes <= 0) {
        throw std::invalid_argument("Invalid backtest capital or position limit");
    }
    const int entryStartMinute =
        parseClockTime(config.entryStartTime, "Backtest entry start");
    const int entryEndMinute =
        parseClockTime(config.entryEndTime, "Backtest entry end");
    if (entryStartMinute >= entryEndMinute) {
        throw std::invalid_argument(
            "Backtest entry window must start before it ends");
    }

    std::vector<BarEvent> events;
    for (const auto& symbolData : candlesBySymbol) {
        if (!universe.contains(symbolData.first)) {
            continue;
        }

        std::vector<HistoricalCandle> candles = symbolData.second;
        std::sort(candles.begin(), candles.end(),
                  [](const HistoricalCandle& left,
                     const HistoricalCandle& right) {
                      return left.timestamp < right.timestamp;
                  });
        std::string previousTimestamp;
        for (const HistoricalCandle& candle : candles) {
            validateCandle(symbolData.first, candle);
            if (candle.timestamp == previousTimestamp) {
                throw std::invalid_argument(
                    "Duplicate historical candle for " + symbolData.first);
            }
            previousTimestamp = candle.timestamp;
            events.push_back({symbolData.first, candle});
        }
    }
    if (events.empty()) {
        throw std::invalid_argument(
            "No historical candles were supplied for the configured universe");
    }

    std::sort(events.begin(), events.end(),
              [](const BarEvent& left, const BarEvent& right) {
                  if (left.candle.timestamp != right.candle.timestamp) {
                      return left.candle.timestamp < right.candle.timestamp;
                  }
                  return left.symbol < right.symbol;
              });

    BacktestResult result;
    result.initialCapital = config.initialCapital;
    result.candleCount = events.size();
    double cash = config.initialCapital;
    double reservedPendingBudget = 0.0;
    double peakEquity = config.initialCapital;
    std::map<std::string, SymbolState> symbolStates;
    std::map<std::string, PendingEntry> pendingEntries;
    std::map<std::string, OpenPosition> openPositions;
    std::map<std::string, std::size_t> filterRejections;

    std::size_t groupStart = 0;
    while (groupStart < events.size()) {
        std::size_t groupEnd = groupStart + 1;
        const std::string timestamp = events[groupStart].candle.timestamp;
        const int candleMinute = candleMinuteOfDay(timestamp);
        const bool entryWindowOpen =
            candleMinute >= entryStartMinute &&
            candleMinute < entryEndMinute;
        while (groupEnd < events.size() &&
               events[groupEnd].candle.timestamp == timestamp) {
            ++groupEnd;
        }

        std::map<std::string, const HistoricalCandle*> bars;
        std::set<std::string> blockedFromReselection;
        for (std::size_t index = groupStart; index < groupEnd; ++index) {
            const BarEvent& event = events[index];
            SymbolState& state = symbolStates[event.symbol];
            if (state.cumulativeVolume >
                std::numeric_limits<std::uint64_t>::max() -
                    event.candle.volume) {
                throw std::overflow_error(
                    "Cumulative historical volume overflow for " +
                    event.symbol);
            }
            state.cumulativeVolume += event.candle.volume;
            state.sessionHigh =
                std::max(state.sessionHigh, event.candle.high);
            state.sessionLow = state.sessionLow == 0.0
                ? event.candle.low
                : std::min(state.sessionLow, event.candle.low);
            state.latestClose = event.candle.close;
            state.latestTimestamp = event.candle.timestamp;
            bars[event.symbol] = &event.candle;
        }

        // Existing positions are checked before new signals, so a freed slot
        // can be reused only on a later candle, not with the exit candle itself.
        std::vector<std::string> positionsToClose;
        for (auto& positionEntry : openPositions) {
            const auto bar = bars.find(positionEntry.first);
            if (bar == bars.end()) {
                continue;
            }
            blockedFromReselection.insert(positionEntry.first);
            const HistoricalCandle& candle = *bar->second;
            const BacktestTrade& trade = positionEntry.second.trade;
            const bool stopTouched =
                trade.side == PositionSide::Long
                    ? candle.low <= trade.stopLossPrice
                    : candle.high >= trade.stopLossPrice;
            const bool targetTouched =
                trade.side == PositionSide::Long
                    ? candle.high >= trade.targetPrice
                    : candle.low <= trade.targetPrice;
            if (stopTouched) {
                // OHLC bars do not show whether target or stop traded first;
                // assume the stop wins if both occur in one candle.
                const double exitPrice =
                    trade.side == PositionSide::Long
                        ? (candle.open < trade.stopLossPrice
                               ? candle.open
                               : trade.stopLossPrice)
                        : (candle.open > trade.stopLossPrice
                               ? candle.open
                               : trade.stopLossPrice);
                closePosition(positionEntry.second,
                              candle.timestamp, exitPrice, "stop_loss",
                              cash, result);
                positionsToClose.push_back(positionEntry.first);
            } else if (targetTouched) {
                const double exitPrice =
                    trade.side == PositionSide::Long
                        ? (candle.open > trade.targetPrice
                               ? candle.open
                               : trade.targetPrice)
                        : (candle.open < trade.targetPrice
                               ? candle.open
                               : trade.targetPrice);
                closePosition(positionEntry.second,
                              candle.timestamp, exitPrice, "target",
                              cash, result);
                positionsToClose.push_back(positionEntry.first);
            }
        }
        for (const std::string& symbol : positionsToClose) {
            openPositions.erase(symbol);
        }

        // Keep pending stop-limit entries eligible until they fill, expire,
        // or the configured session entry window closes.
        std::vector<std::string> pendingToRemove;
        for (auto& pendingEntry : pendingEntries) {
            const auto bar = bars.find(pendingEntry.first);
            if (bar == bars.end()) {
                continue;
            }
            blockedFromReselection.insert(pendingEntry.first);
            const HistoricalCandle& candle = *bar->second;
            PendingEntry& pending = pendingEntry.second;
            const std::string candleDate = candle.timestamp.substr(0, 10);
            const int elapsedMinutes =
                candleMinute - pending.signalMinute;
            if (candleDate == pending.signalDate && elapsedMinutes <= 0) {
                continue;
            }
            const bool sessionEnded = candleDate != pending.signalDate;
            const bool expiredBeforeThisBar =
                !sessionEnded &&
                elapsedMinutes > config.entryTimeoutMinutes;
            if (sessionEnded || expiredBeforeThisBar) {
                ++result.unfilledEntries;
                logLine(
                    "[backtest] entry_unfilled symbol=" +
                    pending.trade.symbol +
                    " time=" + candle.timestamp +
                    " reason=" +
                    (sessionEnded ? "session_ended" : "timeout") +
                    " elapsed_minutes=" + std::to_string(elapsedMinutes));
                reservedPendingBudget -= pending.reservedBudget;
                pendingToRemove.push_back(pendingEntry.first);
                continue;
            }
            const bool gapBeyondLimit =
                pending.trade.side == PositionSide::Long
                    ? candle.open > pending.trade.entryLimitPrice
                    : candle.open < pending.trade.entryLimitPrice;
            const bool triggerReached =
                pending.trade.side == PositionSide::Long
                    ? candle.high >= pending.trade.entryTriggerPrice
                    : candle.low <= pending.trade.entryTriggerPrice;
            const double fillPrice =
                pending.trade.side == PositionSide::Long
                    ? std::max(candle.open,
                               pending.trade.entryTriggerPrice)
                    : std::min(candle.open,
                               pending.trade.entryTriggerPrice);
            if (entryWindowOpen && !gapBeyondLimit && triggerReached &&
                (pending.trade.side == PositionSide::Long
                     ? fillPrice <= pending.trade.entryLimitPrice
                     : fillPrice >= pending.trade.entryLimitPrice)) {
                pending.trade.entryTime = candle.timestamp;
                pending.trade.entryPrice = fillPrice;
                if (pending.trade.side == PositionSide::Long) {
                    cash -= fillPrice * pending.trade.quantity;
                } else {
                    cash += fillPrice * pending.trade.quantity;
                }
                openPositions[pendingEntry.first] =
                    OpenPosition{pending.trade};
                ++result.filledEntries;
                logLine(
                    "[backtest] entry_filled symbol=" + pending.trade.symbol +
                    " side=" +
                    (pending.trade.side == PositionSide::Short
                         ? "SHORT"
                         : "LONG") +
                    " time=" + candle.timestamp +
                    " price=" + std::to_string(fillPrice) +
                    " quantity=" + std::to_string(pending.trade.quantity));
                reservedPendingBudget -= pending.reservedBudget;
                pendingToRemove.push_back(pendingEntry.first);
            } else {
                const bool timeoutReached =
                    elapsedMinutes >= config.entryTimeoutMinutes;
                const bool windowClosed = !entryWindowOpen;
                const std::string reason = windowClosed
                    ? "entry_window_closed"
                    : timeoutReached
                        ? "timeout"
                    : gapBeyondLimit
                        ? "gap_beyond_limit"
                        : !triggerReached
                            ? "trigger_not_reached"
                            : "fill_beyond_limit";
                if (windowClosed || timeoutReached) {
                    ++result.unfilledEntries;
                    logLine(
                        "[backtest] entry_unfilled symbol=" +
                        pending.trade.symbol +
                        " time=" + candle.timestamp +
                        " reason=" + reason +
                        " elapsed_minutes=" +
                        std::to_string(elapsedMinutes) +
                        " open=" + std::to_string(candle.open) +
                        " high=" + std::to_string(candle.high) +
                        " low=" + std::to_string(candle.low) +
                        " trigger=" +
                        std::to_string(pending.trade.entryTriggerPrice) +
                        " limit=" +
                        std::to_string(pending.trade.entryLimitPrice));
                    reservedPendingBudget -= pending.reservedBudget;
                    pendingToRemove.push_back(pendingEntry.first);
                } else {
                    logLine(
                        "[backtest] entry_waiting symbol=" +
                        pending.trade.symbol +
                        " time=" + candle.timestamp +
                        " elapsed_minutes=" +
                        std::to_string(elapsedMinutes) +
                        " timeout_minutes=" +
                        std::to_string(config.entryTimeoutMinutes) +
                        " reason=" + reason +
                        " high=" + std::to_string(candle.high) +
                        " low=" + std::to_string(candle.low) +
                        " trigger=" +
                        std::to_string(pending.trade.entryTriggerPrice) +
                        " limit=" +
                        std::to_string(pending.trade.entryLimitPrice));
                }
            }
        }
        for (const std::string& symbol : pendingToRemove) {
            pendingEntries.erase(symbol);
        }

        for (std::size_t index = groupStart; index < groupEnd; ++index) {
            const BarEvent& event = events[index];
            const std::string& symbol = event.symbol;
            if (blockedFromReselection.find(symbol) !=
                    blockedFromReselection.end() ||
                openPositions.find(symbol) != openPositions.end() ||
                pendingEntries.find(symbol) != pendingEntries.end()) {
                continue;
            }

            SymbolState& state = symbolStates[symbol];
            const HistoricalCandle& candle = event.candle;
            double priceChangePercent = 0.0;
            if (state.previousClose > 0.0 &&
                candle.timestamp > state.previousTimestamp) {
                priceChangePercent =
                    (candle.close - state.previousClose) /
                    state.previousClose * 100.0;
            }
            // Advance the baseline before other gates, matching live selection:
            // rejected bars still become the reference for the next bar.
            if (candle.timestamp > state.previousTimestamp) {
                state.previousClose = candle.close;
                state.previousTimestamp = candle.timestamp;
            }

            const PositionSide side = priceChangePercent < 0.0
                ? PositionSide::Short
                : PositionSide::Long;
            const bool shortDisabled =
                side == PositionSide::Short &&
                (!config.selection.allowShortSelling ||
                 !config.decision.allowShortSelling);
            const bool momentumBelowThreshold =
                side == PositionSide::Short
                    ? priceChangePercent >
                          -config.selection.minPriceChangePercent
                    : priceChangePercent <
                          config.selection.minPriceChangePercent;
            if (candle.close <= config.selection.minPrice) {
                ++filterRejections["min_price"];
                continue;
            }
            if (config.selection.requireNifty100 &&
                !universe.contains(symbol)) {
                ++filterRejections["outside_universe"];
                continue;
            }
            if (state.cumulativeVolume <
                config.selection.minAbsoluteVolume) {
                ++filterRejections["absolute_volume"];
                continue;
            }
            if (shortDisabled) {
                ++filterRejections["short_selling_disabled"];
                continue;
            }

            const double currentVolume =
                static_cast<double>(state.cumulativeVolume);
            if (state.averageVolume <= 0.0) {
                state.averageVolume = currentVolume;
            } else {
                const double alpha = config.selection.volumeEwmaAlpha;
                state.averageVolume =
                    alpha * currentVolume + (1.0 - alpha) * state.averageVolume;
            }
            const double relativeVolume =
                state.averageVolume > 0.0
                    ? currentVolume / state.averageVolume
                    : 1.0;
            const double rangePercent =
                state.sessionLow > 0.0
                    ? (state.sessionHigh - state.sessionLow) /
                          state.sessionLow * 100.0
                    : 0.0;
            if (relativeVolume < config.selection.minRelativeVolume) {
                ++filterRejections["selection_relative_volume"];
                continue;
            }
            if (config.decision.requireVolumeConfirmation &&
                relativeVolume < config.decision.minRelativeVolume) {
                ++filterRejections["decision_relative_volume"];
                continue;
            }
            if (momentumBelowThreshold) {
                ++filterRejections["directional_momentum"];
                continue;
            }
            if (rangePercent < config.selection.minIntradayRangePercent) {
                ++filterRejections["intraday_range_too_small"];
                continue;
            }
            if (rangePercent > config.selection.maxPriceChangePercent ||
                std::abs(priceChangePercent) >
                    config.selection.maxPriceChangePercent) {
                ++filterRejections["intraday_move_too_large"];
                continue;
            }
            if (!entryWindowOpen) {
                ++filterRejections["outside_entry_window"];
                continue;
            }

            // Candle data has no second quote within the same bar. Its close is
            // used for both selection and decision's non-falling-price check.
            ++result.generatedSignals;
            const auto tick = tickSizes.find(symbol);
            const double tickSize =
                tick != tickSizes.end() &&
                        std::isfinite(tick->second) && tick->second > 0.0
                    ? tick->second
                    : config.fallbackTickSize;
            const double trigger =
                side == PositionSide::Long
                    ? roundUpToTick(candle.close, tickSize) +
                          tickSize * static_cast<double>(
                                         config.decision.entryTriggerOffsetTicks)
                    : roundDownToTick(candle.close, tickSize) -
                          tickSize * static_cast<double>(
                                         config.decision.entryTriggerOffsetTicks);
            const double limit = side == PositionSide::Long
                ? trigger + tickSize * static_cast<double>(
                                           config.decision.entryLimitOffsetTicks)
                : trigger - tickSize * static_cast<double>(
                                           config.decision.entryLimitOffsetTicks);

            const std::size_t reservedCount =
                openPositions.size() + pendingEntries.size();
            const double availableCash = cash - reservedPendingBudget;
            const double slotBudget = std::min(
                config.initialCapital /
                    static_cast<double>(config.maxOpenPositions),
                availableCash);
            logLine(
                "[backtest] signal_generated symbol=" + symbol +
                " side=" + (side == PositionSide::Short ? "SHORT" : "LONG") +
                " time=" + candle.timestamp +
                " close=" + std::to_string(candle.close) +
                " trigger=" + std::to_string(trigger) +
                " limit=" + std::to_string(limit) +
                " slot_budget=" + std::to_string(slotBudget));
            if (reservedCount >= config.maxOpenPositions) {
                ++result.riskRejectedSignals;
                ++result.positionLimitRejectedSignals;
                logLine(
                    "[backtest] signal_rejected symbol=" + symbol +
                    " reason=position_limit open_positions=" +
                    std::to_string(reservedCount) +
                    " max_positions=" +
                    std::to_string(config.maxOpenPositions));
                continue;
            }
            if (slotBudget <= 0.0) {
                ++result.riskRejectedSignals;
                ++result.insufficientBudgetRejectedSignals;
                logLine(
                    "[backtest] signal_rejected symbol=" + symbol +
                    " reason=no_available_cash available_cash=" +
                    std::to_string(availableCash));
                continue;
            }

            const double budgetQuantityValue = std::floor(slotBudget / limit);
            if (budgetQuantityValue < 1.0) {
                ++result.riskRejectedSignals;
                ++result.insufficientBudgetRejectedSignals;
                logLine(
                    "[backtest] signal_rejected symbol=" + symbol +
                    " reason=cannot_afford_one_share limit=" +
                    std::to_string(limit) +
                    " slot_budget=" + std::to_string(slotBudget));
                continue;
            }
            const double indicativeValue = std::floor(
                config.decision.indicativeEntryBudget / limit);
            const double cappedValue = std::min(
                budgetQuantityValue,
                indicativeValue < 1.0 ? 1.0 : indicativeValue);
            if (cappedValue >
                static_cast<double>(std::numeric_limits<int>::max())) {
                throw std::overflow_error(
                    "Backtest order quantity exceeds supported range");
            }

            PendingEntry pending;
            pending.trade.symbol = symbol;
            pending.trade.side = side;
            pending.trade.signalTime = candle.timestamp;
            pending.trade.entryTriggerPrice = trigger;
            pending.trade.entryLimitPrice = limit;
            const double targetMultiplier =
                1.0 + config.decision.targetPercent / 100.0;
            const double shortTargetMultiplier =
                1.0 - config.decision.targetPercent / 100.0;
            const double stopMultiplier =
                1.0 - config.decision.stopPercent / 100.0;
            const double shortStopMultiplier =
                1.0 + config.decision.stopPercent / 100.0;
            pending.trade.targetPrice = side == PositionSide::Long
                ? limit * targetMultiplier
                : limit * shortTargetMultiplier;
            pending.trade.stopLossPrice = side == PositionSide::Long
                ? limit * stopMultiplier
                : limit * shortStopMultiplier;
            pending.trade.quantity = static_cast<int>(cappedValue);
            pending.reservedBudget = slotBudget;
            pending.signalDate = candle.timestamp.substr(0, 10);
            pending.signalMinute = candleMinute;
            reservedPendingBudget += slotBudget;
            pendingEntries[symbol] = pending;
            logLine(
                "[backtest] entry_queued symbol=" + symbol +
                " side=" + (side == PositionSide::Short ? "SHORT" : "LONG") +
                " quantity=" + std::to_string(pending.trade.quantity) +
                " reserved_budget=" + std::to_string(slotBudget) +
                " timeout_minutes=" +
                std::to_string(config.entryTimeoutMinutes));
        }

        const double equity = equityAtLatestCloses(
            cash, openPositions, symbolStates);
        peakEquity = std::max(peakEquity, equity);
        if (peakEquity > 0.0) {
            result.maxDrawdownPercent = std::max(
                result.maxDrawdownPercent,
                (peakEquity - equity) / peakEquity * 100.0);
        }
        groupStart = groupEnd;
    }

    // Cancel final-bar signals that had no later candle to test for a fill.
    for (const auto& pending : pendingEntries) {
        ++result.unfilledEntries;
        logLine("[backtest] entry_unfilled symbol=" + pending.first +
                " reason=no_later_candle");
    }

    // Close positions at their final available candle because this is an
    // intraday MIS simulation and the selected date has ended.
    for (auto& positionEntry : openPositions) {
        const SymbolState& state = symbolStates.at(positionEntry.first);
        closePosition(positionEntry.second,
                      state.latestTimestamp, state.latestClose, "end_of_day",
                      cash, result);
    }
    openPositions.clear();

    result.endingCapital = cash;
    result.grossProfitLoss = cash - result.initialCapital;
    const double finalEquity = cash;
    peakEquity = std::max(peakEquity, finalEquity);
    if (peakEquity > 0.0) {
        result.maxDrawdownPercent = std::max(
            result.maxDrawdownPercent,
            (peakEquity - finalEquity) / peakEquity * 100.0);
    }
    for (const auto& rejection : filterRejections) {
        logLine("[backtest] filter_rejected reason=" + rejection.first +
                " count=" + std::to_string(rejection.second));
    }
    logLine(
        "[backtest] completed candles=" + std::to_string(result.candleCount) +
        " signals=" + std::to_string(result.generatedSignals) +
        " fills=" + std::to_string(result.filledEntries) +
        " unfilled=" + std::to_string(result.unfilledEntries) +
        " risk_rejected=" + std::to_string(result.riskRejectedSignals));
    return result;
}

} // namespace zerotick
