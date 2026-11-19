#include "zerotick/Strategy.h"

#include <cmath>

namespace zerotick {

constexpr double OrderImbalanceStrategy::levelWeights_[5];

namespace {

double priceTick(const MarketSnapshot& snapshot) {
    double smallestStep = 0.0;
    for (std::size_t level = 1; level < 5; ++level) {
        const double steps[] = {
            snapshot.bidLevels[level - 1].price -
                snapshot.bidLevels[level].price,
            snapshot.askLevels[level].price -
                snapshot.askLevels[level - 1].price
        };
        for (double step : steps) {
            const double normalizedStep = std::round(step * 100.0) / 100.0;
            if (normalizedStep > 0.0 &&
                (smallestStep == 0.0 || normalizedStep < smallestStep)) {
                smallestStep = normalizedStep;
            }
        }
    }

    return smallestStep > 0.0 ? smallestStep : 0.01;
}

double roundUpToTick(double price, double tick) {
    return std::ceil(price / tick - 1e-9) * tick;
}

} // namespace

void OrderImbalanceStrategy::updateOrderBookMetrics(
    const MarketSnapshot& snapshot) {
    double weightedBidQuantity = 0.0;
    double weightedAskQuantity = 0.0;

    for (std::size_t level = 0; level < 5; ++level) {
        weightedBidQuantity +=
            levelWeights_[level] *
            static_cast<double>(snapshot.bidLevels[level].quantity);
        weightedAskQuantity +=
            levelWeights_[level] *
            static_cast<double>(snapshot.askLevels[level].quantity);
    }

    const double totalWeightedQuantity =
        weightedBidQuantity + weightedAskQuantity;
    orderImbalance_ = totalWeightedQuantity > 0.0
        ? (weightedBidQuantity - weightedAskQuantity) /
            totalWeightedQuantity
        : 0.0;

    const double midPrice = (snapshot.bidPrice + snapshot.askPrice) / 2.0;
    if (snapshot.bidPrice > 0.0 &&
        snapshot.askPrice >= snapshot.bidPrice &&
        midPrice > 0.0) {
        spreadPercent_ =
            (snapshot.askPrice - snapshot.bidPrice) / midPrice * 100.0;
    } else {
        spreadPercent_ = 0.0;
    }
}

StrategyDecision OrderImbalanceStrategy::evaluateEntry(
    const MarketSnapshot& snapshot) {
    updateOrderBookMetrics(snapshot);

    const bool hasValidBook =
        snapshot.bidPrice > 0.0 &&
        snapshot.askPrice >= snapshot.bidPrice &&
        snapshot.bidQuantity > 0 &&
        snapshot.askQuantity > 0;
    const bool hasValidPrice = snapshot.lastPrice > 0.0;
    const bool hasPreviousPrice = hasPreviousPrice_;
    priceChangePercent_ =
        hasPreviousPrice && previousPrice_ > 0.0 && hasValidPrice
        ? (snapshot.lastPrice - previousPrice_) / previousPrice_ * 100.0
        : 0.0;
    const bool priceNotFalling =
        hasPreviousPrice && snapshot.lastPrice >= previousPrice_;

    if (hasValidPrice) {
        previousPrice_ = snapshot.lastPrice;
        hasPreviousPrice_ = true;
    }

    if (!hasValidPrice) {
        consecutiveConfirmations_ = 0;
        lastDecisionReason_ = "invalid or missing last price";
    } else if (!hasValidBook) {
        consecutiveConfirmations_ = 0;
        lastDecisionReason_ = "both sides of order-book depth are required";
    } else if (spreadPercent_ > maximumSpreadPercent_) {
        consecutiveConfirmations_ = 0;
        lastDecisionReason_ = "spread exceeds 0.10% limit";
    } else if (orderImbalance_ < minimumImbalance_) {
        consecutiveConfirmations_ = 0;
        lastDecisionReason_ = "weighted imbalance below +0.05";
    } else if (!hasPreviousPrice) {
        consecutiveConfirmations_ = 0;
        lastDecisionReason_ = "waiting for price confirmation";
    } else if (!priceNotFalling) {
        consecutiveConfirmations_ = 0;
        lastDecisionReason_ = "last price fell from previous snapshot";
    } else {
        ++consecutiveConfirmations_;
        if (consecutiveConfirmations_ == requiredConfirmations_) {
            const double tick = priceTick(snapshot);
            const double triggerBase =
                snapshot.lastPrice > snapshot.askPrice
                ? snapshot.lastPrice
                : snapshot.askPrice;
            entryTriggerPrice_ =
                roundUpToTick(triggerBase, tick) + tick;
            entryLimitPrice_ = entryTriggerPrice_ + tick;
            lastDecisionReason_ =
                "imbalance confirmed; stop-limit entry prices calculated";
            return StrategyDecision::Enter;
        }

        if (consecutiveConfirmations_ < requiredConfirmations_) {
            lastDecisionReason_ =
                "imbalance confirmation " +
                std::to_string(consecutiveConfirmations_) + "/" +
                std::to_string(requiredConfirmations_);
        } else {
            lastDecisionReason_ =
                "entry signal already issued for this confirmation run";
        }
    }

    return StrategyDecision::None;
}

StrategyDecision OrderImbalanceStrategy::evaluateExit(
    const MarketSnapshot& snapshot,
    double entryPrice) {
    updateOrderBookMetrics(snapshot);

    if (entryPrice <= 0.0 || snapshot.lastPrice <= 0.0) {
        priceChangePercent_ = 0.0;
        stopLossTriggered_ = false;
        lastDecisionReason_ = "cannot evaluate exit without valid prices";
        return StrategyDecision::None;
    }

    priceChangePercent_ =
        (snapshot.lastPrice - entryPrice) / entryPrice * 100.0;
    stopLossTriggered_ = false;
    if (priceChangePercent_ >= targetPercent_) {
        lastDecisionReason_ = "0.20% profit target reached";
        return StrategyDecision::Exit;
    }
    if (priceChangePercent_ <= -stopPercent_) {
        stopLossTriggered_ = true;
        lastDecisionReason_ = "0.40% stop loss reached";
        return StrategyDecision::Exit;
    }

    lastDecisionReason_ =
        "price remains inside target and stop-loss bounds";
    return StrategyDecision::None;
}

StrategyDiagnostics OrderImbalanceStrategy::diagnostics() const {
    StrategyDiagnostics result;
    result.orderImbalance = orderImbalance_;
    result.spreadPercent = spreadPercent_;
    result.priceChangePercent = priceChangePercent_;
    result.entryTriggerPrice = entryTriggerPrice_;
    result.entryLimitPrice = entryLimitPrice_;
    result.stopLossTriggered = stopLossTriggered_;
    result.consecutiveConfirmations = consecutiveConfirmations_;
    result.reason = lastDecisionReason_;
    return result;
}

} // namespace zerotick
