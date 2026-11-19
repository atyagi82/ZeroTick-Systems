#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace zerotick {

constexpr std::size_t kMaximumLivePositions = 10;

/**
 * Thread-safe concurrent-position cap. Each reservation is limited to the
 * smaller of current free cash and the startup budget divided by slot count.
 */
class PortfolioRiskManager {
public:
    explicit PortfolioRiskManager(
        double totalAvailableCash,
        std::size_t maximumPositions = kMaximumLivePositions)
        : totalAvailableCash_(totalAvailableCash),
          maximumPositions_(maximumPositions) {
        if (!std::isfinite(totalAvailableCash_) ||
            totalAvailableCash_ < 0.0 ||
            maximumPositions_ == 0) {
            throw std::invalid_argument(
                "Portfolio cash budget and position limit must be valid");
        }
    }

    bool reserveSlot(
        const std::string& symbol,
        double currentFreeCash,
        double& slotBudget) {
        if (symbol.empty() ||
            !std::isfinite(currentFreeCash) ||
            currentFreeCash < 0.0) {
            throw std::invalid_argument(
                "Portfolio reservation requires a symbol and valid free cash");
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (reservations_.find(symbol) != reservations_.end() ||
            reservations_.size() >= maximumPositions_) {
            return false;
        }

        slotBudget = std::min(
            totalAvailableCash_ / maximumPositions_,
            currentFreeCash);
        if (slotBudget <= 0.0) {
            return false;
        }

        reservations_.insert(symbol);
        return true;
    }

    void releaseSlot(const std::string& symbol) {
        std::lock_guard<std::mutex> lock(mutex_);
        reservations_.erase(symbol);
    }

    std::size_t activeReservations() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return reservations_.size();
    }

    double slotBudget() const noexcept {
        return totalAvailableCash_ / maximumPositions_;
    }

private:
    const double totalAvailableCash_;
    const std::size_t maximumPositions_;
    mutable std::mutex mutex_;
    std::unordered_set<std::string> reservations_;
};

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

// Quantity is whole shares and rounds down so its price-based cost stays in
// budget; this helper does not estimate broker-specific MIS margin.
inline int calculateQuantityWithinBudget(
    double availableBudget,
    double perShareRequirement) {
    if (!std::isfinite(availableBudget) ||
        !std::isfinite(perShareRequirement) ||
        availableBudget < 0.0 ||
        perShareRequirement <= 0.0) {
        throw std::invalid_argument(
            "Budget and per-share margin requirement must be valid");
    }

    const double quantity =
        std::floor(availableBudget / perShareRequirement);
    if (quantity >
        static_cast<double>(std::numeric_limits<int>::max())) {
        throw std::overflow_error(
            "Calculated order quantity exceeds the supported integer range");
    }
    return static_cast<int>(quantity);
}

} // namespace zerotick
