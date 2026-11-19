#pragma once

#include "Types.h"

#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

namespace zerotick {

class KiteHttpError : public std::runtime_error {
public:
    KiteHttpError(unsigned int statusCode, const std::string& message)
        : std::runtime_error(message),
          statusCode_(statusCode) {}

    unsigned int statusCode() const noexcept {
        return statusCode_;
    }

private:
    unsigned int statusCode_;
};

/**
 * Market-data and guarded order interface used by stock workers.
 */
class KiteApi {
public:
    virtual ~KiteApi() = default;

    virtual MarketSnapshot getQuote(const std::string& symbol) = 0;
    virtual std::vector<MarketSnapshot> refreshQuotes(
        const std::vector<std::string>& symbols) = 0;
    virtual void ensureNoOpenMisExposure(
        const std::vector<std::string>& symbols) = 0;
    virtual CashAvailability cashAvailability() = 0;
    virtual double estimateMisOrderFunds(
        const std::string& symbol,
        int quantity,
        double triggerPrice,
        double limitPrice) = 0;
    virtual OrderResult placeStopLimitEntry(
        const std::string& symbol,
        int quantity,
        double triggerPrice,
        double limitPrice) = 0;
    virtual OrderResult placeLimitExit(
        const std::string& symbol,
        int quantity,
        double limitPrice) = 0;
    virtual BrokerOrderStatus getOrderStatus(
        const std::string& orderId) = 0;
    virtual void cancelOrder(const std::string& orderId) = 0;

    virtual bool isHealthy() const = 0;
};

/**
 * Kite Connect REST adapter for quote snapshots and MIS limit orders.
 */
class KiteConnectApi : public KiteApi {
public:
    explicit KiteConnectApi(bool forceLogin = false);

    MarketSnapshot getQuote(const std::string& symbol) override;
    std::vector<MarketSnapshot> refreshQuotes(
        const std::vector<std::string>& symbols) override;
    void ensureNoOpenMisExposure(
        const std::vector<std::string>& symbols) override;
    CashAvailability cashAvailability() override;
    double estimateMisOrderFunds(
        const std::string& symbol,
        int quantity,
        double triggerPrice,
        double limitPrice) override;
    OrderResult placeStopLimitEntry(
        const std::string& symbol,
        int quantity,
        double triggerPrice,
        double limitPrice) override;
    OrderResult placeLimitExit(
        const std::string& symbol,
        int quantity,
        double limitPrice) override;
    BrokerOrderStatus getOrderStatus(
        const std::string& orderId) override;
    void cancelOrder(const std::string& orderId) override;
    bool isHealthy() const override;

private:
    std::string apiKey_;
    std::string accessToken_;
    bool healthy_ = false;
    mutable std::mutex quoteMutex_;
    std::map<std::string, MarketSnapshot> quotes_;
    std::set<std::string> missingQuoteSymbols_;
};

} // namespace zerotick
