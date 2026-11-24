#pragma once

#include "BacktestEngine.h"
#include "EngineTypes.h"

#include <chrono>
#include <cstddef>
#include <string>

namespace zerotick {

enum class RunMode {
    Paper,
    ShadowLive,
    Backtest,
    Optimize100Days,
    ExportBacktestData,
    Live
};

struct RunConfig {
    RunMode mode = RunMode::Paper;
    bool forceLogin = false;
    bool liveTradingConfirmed = false;
    std::string backtestDate;
    std::string dataOutputPath = "shadow_live_events.jsonl";
    std::size_t dataOutputMaxMb = 100;
    std::size_t historicalTrainingSessions = 100;
    std::size_t historicalValidationSessions = 100;
};

struct RuntimeConfig {
    RunConfig run;
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
