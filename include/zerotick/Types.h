#pragma once

#include <cstdint>
#include <chrono>
#include <array>
#include <string>

namespace zerotick {

struct OrderBookLevel {
    double price = 0.0;
    std::uint64_t quantity = 0;
};

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
    // Five levels from the exchange response; quantities below also retain
    // the total displayed quantity for logs and basic liquidity checks.
    std::array<OrderBookLevel, 5> bidLevels{};
    std::array<OrderBookLevel, 5> askLevels{};
    std::uint64_t bidQuantity = 0;
    std::uint64_t askQuantity = 0;
    std::uint64_t volume = 0;
    double openPrice = 0.0;
    double highPrice = 0.0;
    double lowPrice = 0.0;
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point receivedAt;
};

struct OrderResult {
    bool accepted = false;
    std::string orderId;
    std::string message;
};

struct BrokerOrderStatus {
    std::string status;
    int filledQuantity = 0;
    double averagePrice = 0.0;
    bool terminal = false;
};

struct CashAvailability {
    double rawCash = 0.0;
    double intradayPayin = 0.0;
    double adhocMargin = 0.0;
    double liveBalance = 0.0;
    double utilizedDebits = 0.0;
    double collateral = 0.0;
    double freeCash = 0.0;
};

} // namespace zerotick
