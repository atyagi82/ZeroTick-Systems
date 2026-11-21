#pragma once

#include "EngineTypes.h"
#include "Risk.h"

#include <map>
#include <string>
#include <vector>

namespace zerotick {

/** Result of simulating one completed intraday position from historical candles. */
struct BacktestTrade {
    std::string symbol;
    PositionSide side = PositionSide::Long;
    std::string signalTime;
    std::string entryTime;
    std::string exitTime;
    std::string exitReason;
    int quantity = 0;
    double entryTriggerPrice = 0.0;
    double entryLimitPrice = 0.0;
    double entryPrice = 0.0;
    double targetPrice = 0.0;
    double stopLossPrice = 0.0;
    double exitPrice = 0.0;
    double grossProfitLoss = 0.0;
};

struct BacktestConfig {
    double initialCapital = 10000.0;
    std::size_t maxOpenPositions = kMaximumLivePositions;
    double fallbackTickSize = 0.01;
    int entryTimeoutMinutes = 5;
    std::string entryStartTime = "12:00";
    std::string entryEndTime = "13:00";
    bool synthesizeOneSecondQuotes = false;
    double syntheticBaseSpreadPercent = 0.02;
    double syntheticVolatilitySpreadFactor = 0.05;
    double syntheticMaximumSpreadPercent = 0.20;
    SelectionConfig selection;
    DecisionConfig decision;
};

struct BacktestResult {
    double initialCapital = 0.0;
    double endingCapital = 0.0;
    double grossProfitLoss = 0.0;
    double maxDrawdownPercent = 0.0;
    std::size_t candleCount = 0;
    std::size_t replayEventCount = 0;
    std::size_t generatedSignals = 0;
    std::size_t filledEntries = 0;
    std::size_t unfilledEntries = 0;
    std::size_t riskRejectedSignals = 0;
    std::size_t positionLimitRejectedSignals = 0;
    std::size_t insufficientBudgetRejectedSignals = 0;
    std::vector<BacktestTrade> trades;
};

/**
 * Replays OHLCV bars through the available strategy gates. Configured runs
 * can expand each minute into a deterministic virtual one-second stream.
 *
 * Historical candles do not contain order-book depth, so displayed depth and
 * weighted imbalance checks are explicitly not simulated. A configurable
 * volatility-based spread estimate can be applied to simulated fills.
 */
class BacktestEngine {
public:
    BacktestResult run(
        const StockUniverse& universe,
        const std::map<std::string, std::vector<HistoricalCandle>>& candles,
        const std::map<std::string, double>& tickSizes,
        const BacktestConfig& config) const;
};

} // namespace zerotick
