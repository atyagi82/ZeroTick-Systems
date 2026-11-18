#pragma once

#include <cstdint>
#include <chrono>
#include <string>

namespace zerotick {

/**
 * Market snapshot returned by a broker or market-data provider.
 *
 * In production this structure should also carry timestamps, bid/ask
 * quantities, tick sequence numbers, and venue information.
 */
struct MarketSnapshot {
    std::string symbol;
    double lastPrice = 0.0;
    double bidPrice = 0.0;
    double askPrice = 0.0;
    // Top-level order-book quantities.  These are required to calculate
    // order-depth imbalance: positive means bid demand, negative means ask supply.
    std::uint64_t bidQuantity = 0;
    std::uint64_t askQuantity = 0;
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point receivedAt;
};

struct OrderResult {
    bool accepted = false;
    std::string orderId;
    std::string message;
};

} // namespace zerotick
