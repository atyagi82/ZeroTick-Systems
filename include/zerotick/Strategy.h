#pragma once

#include "Types.h"

#include <string>

namespace zerotick {

enum class StrategyDecision {
    None,
    Enter,
    Exit
};

struct StrategyDiagnostics {
    bool indicatorsReady = false;
    double fastEma = 0.0;
    double slowEma = 0.0;
    double rsi = 50.0;
    double orderImbalance = 0.0;
    std::string reason;
};

/**
 * Starting strategy contract.
 *
 * The initial implementation deliberately uses conservative placeholder rules.
 * These rules must not be treated as profitable or safe trading advice.
 */
class Strategy {
public:
    virtual ~Strategy() = default;

    // Called while no position is held.
    virtual StrategyDecision evaluateEntry(const MarketSnapshot& snapshot) = 0;

    // Called after an entry is filled.
    virtual StrategyDecision evaluateExit(
        const MarketSnapshot& snapshot,
        double entryPrice) = 0;
    virtual StrategyDiagnostics diagnostics() const = 0;
};

class CombinedStrategy : public Strategy {
public:
    StrategyDecision evaluateEntry(const MarketSnapshot& snapshot) override;
    StrategyDecision evaluateExit(
        const MarketSnapshot& snapshot,
        double entryPrice) override;
    StrategyDiagnostics diagnostics() const override;

private:
    // The strategy is stateful because EMA and Wilder-style RSI values need
    // prior ticks.  One CombinedStrategy instance belongs to exactly one
    // stock worker, so indicators are never shared between symbols.
    void updateIndicators(double price);

    // Moving averages are exponential so recent price activity has more weight.
    static constexpr int fastPeriod_ = 5;
    static constexpr int slowPeriod_ = 20;
    static constexpr int rsiPeriod_ = 14;

    // Entry confirmation thresholds.  These are starting values and are not
    // a claim of historical or live profitability.
    double minimumOrderImbalance_ = 0.20;
    double minimumRsi_ = 55.0;
    double maximumRsi_ = 70.0;

    // Exit thresholds.
    double targetPercent_ = 0.0025;
    double stopPercent_ = 0.0015;
    double exitOrderImbalance_ = -0.25;
    double overboughtRsi_ = 75.0;

    bool initialized_ = false;
    bool rsiInitialized_ = false;
    double fastEma_ = 0.0;
    double slowEma_ = 0.0;
    double previousPrice_ = 0.0;
    double averageGain_ = 0.0;
    double averageLoss_ = 0.0;
    double rsi_ = 50.0;
    double lastOrderImbalance_ = 0.0;
    std::string lastDecisionReason_ = "not evaluated";
};

// Kept as an alias during migration so existing callers compile.
using PlaceholderStrategy = CombinedStrategy;

} // namespace zerotick
