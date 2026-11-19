#pragma once

#include <cmath>
#include <limits>
#include <stdexcept>

namespace zerotick {

constexpr double kMaximumCashUtilization = 0.80;

inline double calculateFreeCash(
    double rawCash,
    double intradayPayin,
    double utilizedDebits,
    double liveBalance) {
    if (!std::isfinite(rawCash) ||
        !std::isfinite(intradayPayin) ||
        !std::isfinite(utilizedDebits) ||
        !std::isfinite(liveBalance) ||
        rawCash < 0.0 ||
        intradayPayin < 0.0 ||
        utilizedDebits < 0.0 ||
        liveBalance < 0.0) {
        throw std::invalid_argument(
            "Kite cash, payin, balance, and debit values must be valid");
    }
    const double creditedCash = rawCash + intradayPayin;
    const double cashAfterUtilization =
        creditedCash > utilizedDebits
        ? creditedCash - utilizedDebits
        : 0.0;
    return cashAfterUtilization < liveBalance
        ? cashAfterUtilization
        : liveBalance;
}

inline int calculateIntradayQuantity(
    double availableCash,
    double limitPrice) {
    if (!std::isfinite(availableCash) ||
        !std::isfinite(limitPrice) ||
        availableCash < 0.0 ||
        limitPrice <= 0.0) {
        throw std::invalid_argument(
            "Available cash and order limit price must be valid");
    }

    const double quantity =
        std::floor(availableCash * kMaximumCashUtilization / limitPrice);
    if (quantity >
        static_cast<double>(std::numeric_limits<int>::max())) {
        throw std::overflow_error(
            "Calculated order quantity exceeds the supported integer range");
    }
    return static_cast<int>(quantity);
}

} // namespace zerotick
