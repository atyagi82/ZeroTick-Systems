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
    double orderImbalance = 0.0;
    double spreadPercent = 0.0;
    double priceChangePercent = 0.0;
    double entryTriggerPrice = 0.0;
    double entryLimitPrice = 0.0;
    bool stopLossTriggered = false;
    std::size_t consecutiveConfirmations = 0;
    std::string reason;
};

/**
 * Strategy interface for a per-symbol worker.
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

class OrderImbalanceStrategy : public Strategy {
public:
    StrategyDecision evaluateEntry(const MarketSnapshot& snapshot) override;
    StrategyDecision evaluateExit(
        const MarketSnapshot& snapshot,
        double entryPrice) override;
    StrategyDiagnostics diagnostics() const override;

private:
    void updateOrderBookMetrics(const MarketSnapshot& snapshot);

    // Recent book levels count more than distant levels. This reduces the
    // influence of liquidity far from the best bid/ask.
    static constexpr double levelWeights_[5] = {
        1.0, 0.8, 0.6, 0.4, 0.2
    };

    double minimumImbalance_ = 0.05;
    double maximumSpreadPercent_ = 0.10;
    double targetPercent_ = 0.20;
    double stopPercent_ = 0.40;
    std::size_t requiredConfirmations_ = 2;
    std::size_t consecutiveConfirmations_ = 0;
    double orderImbalance_ = 0.0;
    double spreadPercent_ = 0.0;
    double priceChangePercent_ = 0.0;
    double entryTriggerPrice_ = 0.0;
    double entryLimitPrice_ = 0.0;
    bool stopLossTriggered_ = false;
    double previousPrice_ = 0.0;
    bool hasPreviousPrice_ = false;
    std::string lastDecisionReason_ = "not evaluated";
};

} // namespace zerotick
