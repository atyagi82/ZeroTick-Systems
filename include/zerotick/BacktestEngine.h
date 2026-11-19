#pragma once

#include "EngineTypes.h"
#include "Risk.h"

#include <map>
#include <string>
#include <vector>

namespace zerotick {

/** Result of simulating one completed long trade from historical candles. */
struct BacktestTrade {
    std::string symbol;
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
    SelectionConfig selection;
    DecisionConfig decision;
};

struct BacktestResult {
    double initialCapital = 0.0;
    double endingCapital = 0.0;
    double grossProfitLoss = 0.0;
    double maxDrawdownPercent = 0.0;
    std::size_t candleCount = 0;
    std::size_t generatedSignals = 0;
    std::size_t filledEntries = 0;
    std::size_t unfilledEntries = 0;
    std::size_t riskRejectedSignals = 0;
    std::vector<BacktestTrade> trades;
};

/**
 * Replays minute OHLCV bars through the available strategy gates.
 *
 * Historical candles do not contain order-book depth, so spread, displayed
 * depth, and weighted imbalance checks are explicitly not simulated.
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
