#include "zerotick/BacktestEngine.h"

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
};

struct OpenPosition {
    BacktestTrade trade;
};

double roundUpToTick(double price, double tickSize) {
    return std::ceil(price / tickSize - 1e-9) * tickSize;
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
    trade.grossProfitLoss =
        (trade.exitPrice - trade.entryPrice) * trade.quantity;
    cash += trade.exitPrice * trade.quantity;
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
            equity += entry.second.trade.quantity * state->second.latestClose;
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
        config.fallbackTickSize <= 0.0) {
        throw std::invalid_argument("Invalid backtest capital or position limit");
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

    std::size_t groupStart = 0;
    while (groupStart < events.size()) {
        std::size_t groupEnd = groupStart + 1;
        const std::string timestamp = events[groupStart].candle.timestamp;
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
            const bool stopTouched = candle.low <= trade.stopLossPrice;
            const bool targetTouched = candle.high >= trade.targetPrice;
            if (stopTouched) {
                // OHLC bars do not show whether target or stop traded first;
                // assume the stop wins if both occur in one candle.
                const double exitPrice =
                    candle.open < trade.stopLossPrice
                        ? candle.open
                        : trade.stopLossPrice;
                closePosition(positionEntry.second,
                              candle.timestamp, exitPrice, "stop_loss",
                              cash, result);
                positionsToClose.push_back(positionEntry.first);
            } else if (targetTouched) {
                const double exitPrice =
                    candle.open > trade.targetPrice
                        ? candle.open
                        : trade.targetPrice;
                closePosition(positionEntry.second,
                              candle.timestamp, exitPrice, "target",
                              cash, result);
                positionsToClose.push_back(positionEntry.first);
            }
        }
        for (const std::string& symbol : positionsToClose) {
            openPositions.erase(symbol);
        }

        // A stop-limit buy is eligible only on the candle after its signal.
        std::vector<std::string> pendingToRemove;
        for (auto& pendingEntry : pendingEntries) {
            const auto bar = bars.find(pendingEntry.first);
            if (bar == bars.end()) {
                continue;
            }
            blockedFromReselection.insert(pendingEntry.first);
            const HistoricalCandle& candle = *bar->second;
            PendingEntry& pending = pendingEntry.second;
            const bool gapBeyondLimit =
                candle.open > pending.trade.entryLimitPrice;
            const bool triggerReached =
                candle.high >= pending.trade.entryTriggerPrice;
            const double fillPrice =
                std::max(candle.open, pending.trade.entryTriggerPrice);
            if (!gapBeyondLimit && triggerReached &&
                fillPrice <= pending.trade.entryLimitPrice) {
                pending.trade.entryTime = candle.timestamp;
                pending.trade.entryPrice = fillPrice;
                cash -= fillPrice * pending.trade.quantity;
                openPositions[pendingEntry.first] =
                    OpenPosition{pending.trade};
                ++result.filledEntries;
            } else {
                ++result.unfilledEntries;
            }
            reservedPendingBudget -= pending.reservedBudget;
            pendingToRemove.push_back(pendingEntry.first);
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

            if (candle.close <= config.selection.minPrice ||
                (config.selection.requireNifty100 &&
                 !universe.contains(symbol)) ||
                state.cumulativeVolume <
                    config.selection.minAbsoluteVolume) {
                continue;
            }

            const double currentVolume =
                static_cast<double>(state.cumulativeVolume);
            if (state.averageVolume <= 0.0) {
                state.averageVolume = currentVolume;
            } else {
                constexpr double alpha = 0.2;
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
            if (relativeVolume < config.selection.minRelativeVolume ||
                (config.decision.requireVolumeConfirmation &&
                 relativeVolume < config.decision.minRelativeVolume) ||
                priceChangePercent <
                    config.selection.minPriceChangePercent ||
                rangePercent < config.selection.minIntradayRangePercent ||
                rangePercent > config.selection.maxPriceChangePercent ||
                std::abs(priceChangePercent) >
                    config.selection.maxPriceChangePercent) {
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
                roundUpToTick(candle.close, tickSize) + tickSize;
            const double limit = trigger + tickSize;

            const std::size_t reservedCount =
                openPositions.size() + pendingEntries.size();
            const double availableCash = cash - reservedPendingBudget;
            const double slotBudget = std::min(
                config.initialCapital /
                    static_cast<double>(config.maxOpenPositions),
                availableCash);
            if (reservedCount >= config.maxOpenPositions ||
                slotBudget <= 0.0) {
                ++result.riskRejectedSignals;
                continue;
            }

            const double budgetQuantityValue = std::floor(slotBudget / limit);
            if (budgetQuantityValue < 1.0) {
                ++result.riskRejectedSignals;
                continue;
            }
            const double indicativeValue = std::floor(50000.0 / limit);
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
            pending.trade.signalTime = candle.timestamp;
            pending.trade.entryTriggerPrice = trigger;
            pending.trade.entryLimitPrice = limit;
            pending.trade.targetPrice =
                limit * (1.0 + config.decision.targetPercent / 100.0);
            pending.trade.stopLossPrice =
                limit * (1.0 - config.decision.stopPercent / 100.0);
            pending.trade.quantity = static_cast<int>(cappedValue);
            pending.reservedBudget = slotBudget;
            reservedPendingBudget += slotBudget;
            pendingEntries[symbol] = pending;
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
    result.unfilledEntries += pendingEntries.size();

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
    return result;
}

} // namespace zerotick
