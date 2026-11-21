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
    double spreadPercent = 0.0;
};

struct SymbolState {
    std::uint64_t cumulativeVolume = 0;
    double averageVolume = 0.0;
    double previousClose = 0.0;
    std::string previousTimestamp;
    double sessionHigh = 0.0;
    double sessionLow = 0.0;
    double latestClose = 0.0;
    double latestSpreadPercent = 0.0;
    std::string latestTimestamp;
};

struct PendingEntry {
    BacktestTrade trade;
    double reservedBudget = 0.0;
    std::string signalDate;
    int signalSecond = 0;
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

int candleSecondOfDay(const std::string& timestamp) {
    if (timestamp.size() < 19 || timestamp[10] != 'T' ||
        timestamp[13] != ':' || timestamp[16] != ':') {
        throw std::invalid_argument(
            "Historical candle timestamp must include ISO HH:MM:SS time");
    }
    const int minute = parseClockTime(timestamp.substr(11, 5),
                                      "Historical candle time");
    if (timestamp[17] < '0' || timestamp[17] > '9' ||
        timestamp[18] < '0' || timestamp[18] > '9') {
        throw std::invalid_argument("Historical candle has invalid seconds");
    }
    const int second = (timestamp[17] - '0') * 10 + timestamp[18] - '0';
    if (second > 59) {
        throw std::invalid_argument("Historical candle seconds exceed 59");
    }
    return minute * 60 + second;
}

std::string timestampAtSecond(const std::string& timestamp, int second) {
    std::string result = timestamp;
    result[17] = static_cast<char>('0' + second / 10);
    result[18] = static_cast<char>('0' + second % 10);
    return result;
}

double interpolate(double left, double right, double fraction) {
    return left + (right - left) * fraction;
}

std::uint64_t stableHash(const std::string& value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<HistoricalCandle> synthesizeSeconds(
    const std::string& symbol, const HistoricalCandle& minute) {
    constexpr int count = 60;
    const std::uint64_t seed = stableHash(symbol + minute.timestamp);
    const bool highFirst = (seed & 1ULL) != 0;
    const double firstExtreme = highFirst ? minute.high : minute.low;
    const double secondExtreme = highFirst ? minute.low : minute.high;
    const double anchors[] = {
        minute.open, firstExtreme, secondExtreme, minute.close
    };
    const int anchorSeconds[] = {0, 20, 40, 59};
    const double range = minute.high - minute.low;
    std::vector<HistoricalCandle> seconds;
    seconds.reserve(count);
    std::vector<double> volumeWeights;
    volumeWeights.reserve(count);
    double totalVolumeWeight = 0.0;
    double previous = minute.open;
    for (int second = 0; second < count; ++second) {
        int segment = second <= 20 ? 0 : second <= 40 ? 1 : 2;
        const int segmentStart = anchorSeconds[segment];
        const int segmentEnd = anchorSeconds[segment + 1];
        const double fraction = segmentEnd > segmentStart
            ? static_cast<double>(second - segmentStart) /
                  static_cast<double>(segmentEnd - segmentStart)
            : 0.0;
        double close = interpolate(
            anchors[segment], anchors[segment + 1], fraction);
        if (second != segmentStart && second != segmentEnd && range > 0.0) {
            const double envelope = std::sin(3.141592653589793 * fraction);
            const double wave = std::sin(
                (static_cast<double>(second) +
                 static_cast<double>(seed % 23ULL)) * 2.399963229728653);
            close += range * 0.04 * envelope * wave;
            close = std::max(minute.low, std::min(minute.high, close));
        }
        if (second == 0) close = minute.open;
        if (second == 20) close = firstExtreme;
        if (second == 40) close = secondExtreme;
        if (second == 59) close = minute.close;

        HistoricalCandle generated;
        generated.timestamp = timestampAtSecond(minute.timestamp, second);
        generated.open = previous;
        generated.close = close;
        generated.high = std::max(previous, close);
        generated.low = std::min(previous, close);
        const std::uint64_t volumeSeed = stableHash(
            symbol + minute.timestamp + std::to_string(second));
        const double randomWeight =
            static_cast<double>(volumeSeed % 1000ULL) / 999.0;
        const double moveFraction = range > 0.0
            ? std::abs(close - previous) / range
            : 0.0;
        const double volumeWeight =
            0.20 + 2.0 * randomWeight + 8.0 * moveFraction;
        volumeWeights.push_back(volumeWeight);
        totalVolumeWeight += volumeWeight;
        seconds.push_back(generated);
        previous = close;
    }
    double cumulativeWeight = 0.0;
    std::uint64_t assignedVolume = 0;
    for (int second = 0; second < count; ++second) {
        cumulativeWeight += volumeWeights[second];
        const std::uint64_t targetVolume = second + 1 == count
            ? minute.volume
            : static_cast<std::uint64_t>(
                  static_cast<double>(minute.volume) * cumulativeWeight /
                  totalVolumeWeight);
        seconds[second].volume = targetVolume - assignedVolume;
        assignedVolume = targetVolume;
    }
    return seconds;
}

double estimatedSpreadPercent(
    const HistoricalCandle& candle, const BacktestConfig& config) {
    const double rangePercent = candle.low > 0.0
        ? (candle.high - candle.low) / candle.low * 100.0
        : 0.0;
    return std::min(
        config.syntheticMaximumSpreadPercent,
        config.syntheticBaseSpreadPercent +
            rangePercent * config.syntheticVolatilitySpreadFactor);
}

double executablePrice(double mid, double spreadPercent, PositionSide side) {
    const double halfSpread = spreadPercent / 200.0;
    return side == PositionSide::Long
        ? mid * (1.0 + halfSpread)
        : mid * (1.0 - halfSpread);
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
        config.entryTimeoutMinutes <= 0 ||
        !std::isfinite(config.syntheticBaseSpreadPercent) ||
        !std::isfinite(config.syntheticVolatilitySpreadFactor) ||
        !std::isfinite(config.syntheticMaximumSpreadPercent) ||
        config.syntheticBaseSpreadPercent < 0.0 ||
        config.syntheticVolatilitySpreadFactor < 0.0 ||
        config.syntheticMaximumSpreadPercent <
            config.syntheticBaseSpreadPercent) {
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
    std::size_t sourceCandleCount = 0;
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
            ++sourceCandleCount;
            if (candle.timestamp == previousTimestamp) {
                throw std::invalid_argument(
                    "Duplicate historical candle for " + symbolData.first);
            }
            previousTimestamp = candle.timestamp;
            const double spread = estimatedSpreadPercent(candle, config);
            if (config.synthesizeOneSecondQuotes) {
                const std::vector<HistoricalCandle> generated =
                    synthesizeSeconds(symbolData.first, candle);
                for (const HistoricalCandle& second : generated) {
                    events.push_back({symbolData.first, second, spread});
                }
            } else {
                events.push_back({symbolData.first, candle, spread});
            }
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
    result.candleCount = sourceCandleCount;
    result.replayEventCount = events.size();
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
        const int candleSecond = candleSecondOfDay(timestamp);
        const bool entryWindowOpen =
            candleSecond >= entryStartMinute * 60 &&
            candleSecond < entryEndMinute * 60;
        while (groupEnd < events.size() &&
               events[groupEnd].candle.timestamp == timestamp) {
            ++groupEnd;
        }

        std::map<std::string, const BarEvent*> bars;
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
            state.latestSpreadPercent = event.spreadPercent;
            state.latestTimestamp = event.candle.timestamp;
            bars[event.symbol] = &event;
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
            const BarEvent& barEvent = *bar->second;
            const HistoricalCandle& candle = barEvent.candle;
            const BacktestTrade& trade = positionEntry.second.trade;
            const PositionSide exitTransaction =
                trade.side == PositionSide::Long
                    ? PositionSide::Short : PositionSide::Long;
            const double executableLow = executablePrice(
                candle.low, barEvent.spreadPercent, exitTransaction);
            const double executableHigh = executablePrice(
                candle.high, barEvent.spreadPercent, exitTransaction);
            const double executableOpen = executablePrice(
                candle.open, barEvent.spreadPercent, exitTransaction);
            const bool stopTouched =
                trade.side == PositionSide::Long
                    ? executableLow <= trade.stopLossPrice
                    : executableHigh >= trade.stopLossPrice;
            const bool targetTouched =
                trade.side == PositionSide::Long
                    ? executableHigh >= trade.targetPrice
                    : executableLow <= trade.targetPrice;
            if (stopTouched) {
                // OHLC bars do not show whether target or stop traded first;
                // assume the stop wins if both occur in one candle.
                const double exitPrice =
                    trade.side == PositionSide::Long
                        ? (executableOpen < trade.stopLossPrice
                               ? executableOpen
                               : trade.stopLossPrice)
                        : (executableOpen > trade.stopLossPrice
                               ? executableOpen
                               : trade.stopLossPrice);
                closePosition(positionEntry.second,
                              candle.timestamp, exitPrice, "stop_loss",
                              cash, result);
                positionsToClose.push_back(positionEntry.first);
            } else if (targetTouched) {
                const double exitPrice =
                    trade.side == PositionSide::Long
                        ? (executableOpen > trade.targetPrice
                               ? executableOpen
                               : trade.targetPrice)
                        : (executableOpen < trade.targetPrice
                               ? executableOpen
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
            const BarEvent& barEvent = *bar->second;
            const HistoricalCandle& candle = barEvent.candle;
            PendingEntry& pending = pendingEntry.second;
            const std::string candleDate = candle.timestamp.substr(0, 10);
            const int elapsedSeconds =
                candleSecond - pending.signalSecond;
            if (candleDate == pending.signalDate && elapsedSeconds <= 0) {
                continue;
            }
            const bool sessionEnded = candleDate != pending.signalDate;
            const bool expiredBeforeThisBar =
                !sessionEnded &&
                elapsedSeconds > config.entryTimeoutMinutes * 60;
            if (sessionEnded || expiredBeforeThisBar) {
                ++result.unfilledEntries;
                logLine(
                    "[backtest] entry_unfilled symbol=" +
                    pending.trade.symbol +
                    " time=" + candle.timestamp +
                    " reason=" +
                    (sessionEnded ? "session_ended" : "timeout") +
                    " elapsed_seconds=" + std::to_string(elapsedSeconds));
                reservedPendingBudget -= pending.reservedBudget;
                pendingToRemove.push_back(pendingEntry.first);
                continue;
            }
            const double executableOpen = executablePrice(
                candle.open, barEvent.spreadPercent, pending.trade.side);
            const bool gapBeyondLimit =
                pending.trade.side == PositionSide::Long
                    ? executableOpen > pending.trade.entryLimitPrice
                    : executableOpen < pending.trade.entryLimitPrice;
            const bool triggerReached =
                pending.trade.side == PositionSide::Long
                    ? candle.high >= pending.trade.entryTriggerPrice
                    : candle.low <= pending.trade.entryTriggerPrice;
            const double executableTrigger = executablePrice(
                pending.trade.entryTriggerPrice,
                barEvent.spreadPercent, pending.trade.side);
            const double fillPrice =
                pending.trade.side == PositionSide::Long
                    ? std::max(executableOpen, executableTrigger)
                    : std::min(executableOpen, executableTrigger);
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
                    elapsedSeconds >= config.entryTimeoutMinutes * 60;
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
                        " elapsed_seconds=" +
                        std::to_string(elapsedSeconds) +
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
                        " elapsed_seconds=" +
                        std::to_string(elapsedSeconds) +
                        " timeout_seconds=" +
                        std::to_string(config.entryTimeoutMinutes * 60) +
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
                static_cast<double>(candle.volume);
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
            pending.signalSecond = candleSecond;
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
        const PositionSide exitTransaction =
            positionEntry.second.trade.side == PositionSide::Long
                ? PositionSide::Short : PositionSide::Long;
        const double exitPrice = executablePrice(
            state.latestClose, state.latestSpreadPercent, exitTransaction);
        closePosition(positionEntry.second,
                      state.latestTimestamp, exitPrice, "end_of_day",
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
        " replay_events=" + std::to_string(result.replayEventCount) +
        " one_second_replay=" +
        (config.synthesizeOneSecondQuotes ? "enabled" : "disabled") +
        " signals=" + std::to_string(result.generatedSignals) +
        " fills=" + std::to_string(result.filledEntries) +
        " unfilled=" + std::to_string(result.unfilledEntries) +
        " risk_rejected=" + std::to_string(result.riskRejectedSignals));
    return result;
}

} // namespace zerotick
