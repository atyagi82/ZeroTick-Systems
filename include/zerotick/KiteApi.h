#pragma once

#include "Types.h"

#include <map>
#include <mutex>
#include <vector>

namespace zerotick {

/**
 * Read-only market-data interface used by stock workers.
 */
class KiteApi {
public:
    virtual ~KiteApi() = default;

    virtual MarketSnapshot getQuote(const std::string& symbol) = 0;
    virtual void refreshQuotes(const std::vector<std::string>& symbols) = 0;
    virtual OrderResult placeMarketOrder(
        const std::string& symbol,
        bool buy,
        int quantity) = 0;
    virtual OrderResult exitPosition(
        const std::string& symbol,
        int quantity) = 0;

    virtual bool isHealthy() const = 0;
};

/**
 * Kite Connect REST adapter for authenticated quote snapshots.
 * Live order placement remains disabled.
 */
class KiteConnectApi : public KiteApi {
public:
    explicit KiteConnectApi(bool forceLogin = false);

    MarketSnapshot getQuote(const std::string& symbol) override;
    void refreshQuotes(const std::vector<std::string>& symbols) override;
    OrderResult placeMarketOrder(
        const std::string& symbol,
        bool buy,
        int quantity) override;
    OrderResult exitPosition(
        const std::string& symbol,
        int quantity) override;
    bool isHealthy() const override;

private:
    std::string apiKey_;
    std::string accessToken_;
    bool healthy_ = false;
    mutable std::mutex quoteMutex_;
    std::map<std::string, MarketSnapshot> quotes_;
};

} // namespace zerotick
