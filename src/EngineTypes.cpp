#include "zerotick/EngineTypes.h"
#include "zerotick/Log.h"

#include <algorithm>

namespace zerotick {

NiftyUniverse::NiftyUniverse() {
    // NSE NIFTY 100 constituents from the official list as of 2026-11-19.
    // https://nsearchives.nseindia.com/content/indices/ind_nifty100list.csv
    const char* symbols[] = {
        "ABB", "ADANIENSOL", "ADANIENT", "ADANIGREEN", "ADANIPORTS",
        "ADANIPOWER", "AMBUJACEM", "APOLLOHOSP", "ASIANPAINT", "AXISBANK",
        "BAJAJ-AUTO", "BAJAJFINSV", "BAJAJHLDNG", "BAJFINANCE", "BANKBARODA",
        "BEL", "BHARTIARTL", "BOSCHLTD", "BPCL", "BRITANNIA", "BSE", "CANBK",
        "CGPOWER", "CHOLAFIN", "CIPLA", "COALINDIA", "CUMMINSIND", "DIVISLAB",
        "DLF", "DMART", "DRREDDY", "EICHERMOT", "ENRIN", "ETERNAL", "GAIL",
        "GODREJCP", "GRASIM", "HAL", "HCLTECH", "HDFCAMC", "HDFCBANK",
        "HDFCLIFE", "HINDALCO", "HINDUNILVR", "HINDZINC", "HYUNDAI",
        "ICICIBANK", "IDEA", "INDIGO", "INFY", "IOC", "IRFC", "ITC",
        "JINDALSTEL", "JIOFIN", "JSWSTEEL", "KOTAKBANK", "LT", "LTM", "M&M",
        "MARUTI", "MAXHEALTH", "MAZDOCK", "MOTHERSON", "MUTHOOTFIN",
        "NESTLEIND", "NTPC", "ONGC", "PFC", "PIDILITIND", "PNB", "POLYCAB",
        "POWERGRID", "POWERINDIA", "RELIANCE", "SBILIFE", "SBIN", "SHRIRAMFIN",
        "SIEMENS", "SOLARINDS", "SUNPHARMA", "TATACAP", "TATACONSUM",
        "TATAPOWER", "TATASTEEL", "TCS", "TECHM", "TITAN", "TMCV", "TMPV",
        "TORNTPHARM", "TRENT", "TVSMOTOR", "ULTRACEMCO", "UNIONBANK", "VAML",
        "VBL", "VEDL", "WIPRO", "ZYDUSLIFE"
    };
    for (const char* symbol : symbols) {
        symbols_.push_back(symbol);
    }
    std::sort(symbols_.begin(), symbols_.end());
    symbols_.erase(std::unique(symbols_.begin(), symbols_.end()),
                   symbols_.end());
    std::lock_guard<std::mutex> lock(mutex_);
    set_.insert(symbols_.begin(), symbols_.end());
}

std::vector<std::string> NiftyUniverse::symbols() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return symbols_;
}

bool NiftyUniverse::contains(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return set_.find(symbol) != set_.end();
}

bool StockEngineRegistry::claim(const std::string& symbol,
                                EngineId expectedOwner,
                                EngineId newOwner) {
    if (symbol.empty() || expectedOwner == newOwner) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = ownership_.find(symbol);
    if (found == ownership_.end()) {
        if (expectedOwner == EngineId::Unassigned) {
            ownership_[symbol] = newOwner;
            return true;
        }
        return false; // symbol not tracked yet
    }
    if (found->second != expectedOwner) {
        return false; // owned by another engine (or same)
    }
    found->second = newOwner;
    return true;
}

bool StockEngineRegistry::release(const std::string& symbol,
                                  EngineId owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = ownership_.find(symbol);
    if (found == ownership_.end() || found->second != owner) {
        return false;
    }
    ownership_.erase(found);
    return true;
}

EngineId StockEngineRegistry::ownerOf(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = ownership_.find(symbol);
    if (found == ownership_.end()) {
        return EngineId::Unassigned;
    }
    return found->second;
}

bool StockEngineRegistry::isUnassigned(const std::string& symbol) const {
    return ownerOf(symbol) == EngineId::Unassigned;
}

std::size_t StockEngineRegistry::countInEngine(EngineId engine) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& entry : ownership_) {
        if (entry.second == engine) {
            ++count;
        }
    }
    return count;
}

void StockEngineRegistry::releaseAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    ownership_.clear();
}

} // namespace zerotick
