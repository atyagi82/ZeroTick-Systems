#pragma once

#include "BacktestEngine.h"
#include "EngineTypes.h"

#include <chrono>
#include <cstddef>
#include <string>

namespace zerotick {

struct RuntimeConfig {
    SelectionConfig selection;
    DecisionConfig decision;
    HandlingConfig handling;
    BacktestConfig backtest;
    std::size_t maxOpenPositions = kMaximumLivePositions;
    std::chrono::milliseconds quotePollInterval{1000};
    double paperTradingCash = 100000.0;
    double fallbackTickSize = 0.01;
    bool allowShortSelling = false;
};

RuntimeConfig parseRuntimeConfig(const std::string& contents);
RuntimeConfig loadRuntimeConfig(const std::string& path);

} // namespace zerotick
