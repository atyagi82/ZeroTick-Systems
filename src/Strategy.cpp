#include "zerotick/Strategy.h"

#include <algorithm>
#include <cmath>

namespace zerotick {

void CombinedStrategy::updateIndicators(double price) {
    // First tick initializes both EMAs and also establishes RSI's previous price.
    // A zero or invalid price is ignored rather than allowing an invalid indicator.
    if (!(price > 0.0)) {
        return;
    }

    if (!initialized_) {
        fastEma_ = price;
        slowEma_ = price;
        previousPrice_ = price;
        initialized_ = true;
        rsiInitialized_ = false;
        return;
    }

    // Standard EMA formula: multiplier = 2 / (period + 1).
    const double fastMultiplier = 2.0 / (fastPeriod_ + 1.0);
    const double slowMultiplier = 2.0 / (slowPeriod_ + 1.0);
    fastEma_ = (price - fastEma_) * fastMultiplier + fastEma_;
    slowEma_ = (price - slowEma_) * slowMultiplier + slowEma_;

    // Wilder RSI: separate positive and negative one-tick price changes feed
    // rolling averages, where smoothed loss/gain replaces the simple mean.
    const double change = price - previousPrice_;
    const double gain = std::max(change, 0.0);
    const double loss = std::max(-change, 0.0);
    previousPrice_ = price;

    // EMA smoothing of gains/losses is a practical Wilder approximation.
    // The first values establish a baseline; later ticks smooth it.
    const double rsiMultiplier = 2.0 / (rsiPeriod_ + 1.0);
    if (averageGain_ == 0.0 && averageLoss_ == 0.0) {
        averageGain_ = gain;
        averageLoss_ = loss;
    } else {
        averageGain_ =
            (gain - averageGain_) * rsiMultiplier + averageGain_;
        averageLoss_ =
            (loss - averageLoss_) * rsiMultiplier + averageLoss_;
    }

    rsiInitialized_ = true;
    if (averageLoss_ <= 0.0) {
        rsi_ = 100.0;
    } else {
        const double relativeStrength =
            averageGain_ / averageLoss_;
        rsi_ = 100.0 - (100.0 / (1.0 + relativeStrength));
    }
}

StrategyDecision CombinedStrategy::evaluateEntry(
    const MarketSnapshot& snapshot) {
    updateIndicators(snapshot.lastPrice);

    // The imbalance ranges from -1.0 (ask-heavy) to +1.0 (bid-heavy).
    const std::uint64_t totalQuantity =
        snapshot.bidQuantity + snapshot.askQuantity;
    if (totalQuantity == 0) {
        lastOrderImbalance_ = 0.0;
    } else {
        lastOrderImbalance_ =
            static_cast<double>(snapshot.bidQuantity) /
                static_cast<double>(totalQuantity) -
            static_cast<double>(snapshot.askQuantity) /
                static_cast<double>(totalQuantity);
    }

    // Record the first unmet condition so the worker can explain why it did
    // not produce an entry signal on this quote.
    if (!(snapshot.lastPrice > 0.0)) {
        lastDecisionReason_ = "invalid or missing last price";
    } else if (totalQuantity == 0) {
        lastDecisionReason_ = "order-book depth unavailable";
    } else if (!initialized_ || !rsiInitialized_) {
        lastDecisionReason_ = "indicator warm-up";
    } else if (fastEma_ <= slowEma_) {
        lastDecisionReason_ = "fast EMA is not above slow EMA";
    } else if (rsi_ < minimumRsi_) {
        lastDecisionReason_ = "RSI below entry range";
    } else if (rsi_ > maximumRsi_) {
        lastDecisionReason_ = "RSI above entry range";
    } else if (lastOrderImbalance_ < minimumOrderImbalance_) {
        lastDecisionReason_ = "bid depth imbalance below threshold";
    } else {
        lastDecisionReason_ = "all entry filters passed";
        return StrategyDecision::Enter;
    }

    return StrategyDecision::None;
}

StrategyDecision CombinedStrategy::evaluateExit(
    const MarketSnapshot& snapshot,
    double entryPrice) {
    // Update every indicator even after entry.  This allows the combined
    // momentum, trend, and depth exit rules to use the latest market state.
    updateIndicators(snapshot.lastPrice);

    // Recalculate imbalance before a position exit decision.
    const std::uint64_t totalQuantity =
        snapshot.bidQuantity + snapshot.askQuantity;
    if (totalQuantity != 0) {
        lastOrderImbalance_ =
            static_cast<double>(snapshot.bidQuantity) /
                static_cast<double>(totalQuantity) -
            static_cast<double>(snapshot.askQuantity) /
                static_cast<double>(totalQuantity);
    }

    // Invalid entry price is not a real position state; fail safely and exit.
    if (entryPrice <= 0.0) {
        lastDecisionReason_ = "invalid entry price; exit required";
        return StrategyDecision::Exit;
    }

    const double change = (snapshot.lastPrice - entryPrice) / entryPrice;

    // Fixed target and stop protect the position against small moves.
    if (change >= targetPercent_) {
        lastDecisionReason_ = "profit target reached";
        return StrategyDecision::Exit;
    }
    if (change <= -stopPercent_) {
        lastDecisionReason_ = "stop loss reached";
        return StrategyDecision::Exit;
    }

    // Momentum/reversal exits:
    // - RSI is overbought and may be exhausted.
    // - fast EMA has crossed below slow EMA.
    // - seller-side depth imbalance is strong enough.
    if (rsi_ >= overboughtRsi_) {
        lastDecisionReason_ = "RSI overbought exit";
        return StrategyDecision::Exit;
    }
    if (initialized_ && fastEma_ < slowEma_) {
        lastDecisionReason_ = "bearish EMA exit";
        return StrategyDecision::Exit;
    }
    if (lastOrderImbalance_ <= exitOrderImbalance_) {
        lastDecisionReason_ = "ask-heavy depth exit";
        return StrategyDecision::Exit;
    }

    lastDecisionReason_ = "exit conditions not met";
    return StrategyDecision::None;
}

StrategyDiagnostics CombinedStrategy::diagnostics() const {
    StrategyDiagnostics result;
    result.indicatorsReady = initialized_ && rsiInitialized_;
    result.fastEma = fastEma_;
    result.slowEma = slowEma_;
    result.rsi = rsi_;
    result.orderImbalance = lastOrderImbalance_;
    result.reason = lastDecisionReason_;
    return result;
}

} // namespace zerotick
