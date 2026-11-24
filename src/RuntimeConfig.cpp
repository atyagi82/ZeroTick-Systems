#include "zerotick/RuntimeConfig.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace zerotick {
namespace {

using Json = nlohmann::json;

const Json& objectSection(const Json& root, const char* name) {
    static const Json emptyObject = Json::object();
    const auto section = root.find(name);
    if (section == root.end()) {
        return emptyObject;
    }
    if (!section->is_object()) {
        throw std::invalid_argument(
            std::string("Configuration section '") + name +
            "' must be an object");
    }
    return *section;
}

void rejectUnknownKeys(
    const Json& object,
    std::initializer_list<const char*> allowedKeys,
    const std::string& sectionName) {
    for (auto entry = object.begin(); entry != object.end(); ++entry) {
        bool known = false;
        for (const char* key : allowedKeys) {
            if (entry.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            throw std::invalid_argument(
                "Unknown configuration key: " + sectionName + "." +
                entry.key());
        }
    }
}

template <typename T>
void readValue(const Json& object,
               const char* key,
               T& destination,
               const std::string& sectionName) {
    const auto entry = object.find(key);
    if (entry == object.end()) {
        return;
    }
    try {
        destination = entry->get<T>();
    } catch (const Json::exception& exception) {
        throw std::invalid_argument(
            "Invalid configuration value for " + sectionName + "." + key +
            ": " + exception.what());
    }
}

template <typename T>
void readUnsignedValue(const Json& object,
                       const char* key,
                       T& destination,
                       const std::string& sectionName) {
    const auto entry = object.find(key);
    if (entry == object.end()) {
        return;
    }
    if (!entry->is_number_integer()) {
        throw std::invalid_argument(
            "Configuration value " + sectionName + "." + key +
            " must be a nonnegative integer");
    }
    std::uint64_t value = 0;
    if (entry->is_number_unsigned()) {
        value = entry->get<std::uint64_t>();
    } else {
        const std::int64_t signedValue = entry->get<std::int64_t>();
        if (signedValue < 0) {
            throw std::invalid_argument(
                "Configuration value " + sectionName + "." + key +
                " must be a nonnegative integer");
        }
        value = static_cast<std::uint64_t>(signedValue);
    }
    if (value > static_cast<std::uint64_t>(
                    std::numeric_limits<T>::max())) {
        throw std::invalid_argument(
            "Configuration value " + sectionName + "." + key +
            " exceeds the supported range");
    }
    destination = static_cast<T>(value);
}

void readIntValue(const Json& object,
                  const char* key,
                  int& destination,
                  const std::string& sectionName) {
    const auto entry = object.find(key);
    if (entry == object.end()) {
        return;
    }
    if (!entry->is_number_integer()) {
        throw std::invalid_argument(
            "Configuration value " + sectionName + "." + key +
            " must be an integer");
    }
    std::int64_t value = 0;
    if (entry->is_number_unsigned()) {
        const std::uint64_t unsignedValue = entry->get<std::uint64_t>();
        if (unsignedValue >
            static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument(
                "Configuration value " + sectionName + "." + key +
                " exceeds the supported range");
        }
        value = static_cast<std::int64_t>(unsignedValue);
    } else {
        value = entry->get<std::int64_t>();
    }
    if (value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        throw std::invalid_argument(
            "Configuration value " + sectionName + "." + key +
            " exceeds the supported range");
    }
    destination = static_cast<int>(value);
}

void requireFinitePositive(double value, const std::string& name) {
    if (!std::isfinite(value) || value <= 0.0) {
        throw std::invalid_argument(
            "Configuration value " + name + " must be finite and positive");
    }
}

void requireFiniteNonnegative(double value, const std::string& name) {
    if (!std::isfinite(value) || value < 0.0) {
        throw std::invalid_argument(
            "Configuration value " + name +
            " must be finite and nonnegative");
    }
}

void validateClockTime(const std::string& value, const std::string& name) {
    if (value.size() != 5 || value[2] != ':' ||
        value[0] < '0' || value[0] > '9' ||
        value[1] < '0' || value[1] > '9' ||
        value[3] < '0' || value[3] > '9' ||
        value[4] < '0' || value[4] > '9') {
        throw std::invalid_argument(
            "Configuration value " + name + " must use HH:MM format");
    }
    const int hour = (value[0] - '0') * 10 + value[1] - '0';
    const int minute = (value[3] - '0') * 10 + value[4] - '0';
    if (hour > 23 || minute > 59) {
        throw std::invalid_argument(
            "Configuration value " + name + " is outside the clock range");
    }
}

RunMode parseRunMode(const std::string& value) {
    if (value == "paper") return RunMode::Paper;
    if (value == "shadow_live") return RunMode::ShadowLive;
    if (value == "backtest") return RunMode::Backtest;
    if (value == "optimize_100_days") return RunMode::Optimize100Days;
    if (value == "export_backtest_data") return RunMode::ExportBacktestData;
    if (value == "live") return RunMode::Live;
    throw std::invalid_argument(
        "run.mode must be paper, shadow_live, backtest, "
        "optimize_100_days, export_backtest_data, or live");
}

void validateBacktestDate(const std::string& value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
        throw std::invalid_argument(
            "run.backtest_date must use YYYY-MM-DD format");
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index != 4 && index != 7 &&
            (value[index] < '0' || value[index] > '9')) {
            throw std::invalid_argument(
                "run.backtest_date must use YYYY-MM-DD format");
        }
    }
}

} // namespace

RuntimeConfig parseRuntimeConfig(const std::string& contents) {
    Json root;
    try {
        root = Json::parse(contents);
    } catch (const Json::parse_error& exception) {
        throw std::invalid_argument(
            std::string("Configuration is not valid JSON: ") +
            exception.what());
    }
    if (!root.is_object()) {
        throw std::invalid_argument(
            "The configuration root must be a JSON object");
    }
    rejectUnknownKeys(
        root,
        {"run", "allow_short_selling", "market_data", "selection", "decision",
         "handling", "risk", "backtest"},
        "root");

    RuntimeConfig config;

    const Json& run = objectSection(root, "run");
    rejectUnknownKeys(
        run,
        {"mode", "force_login", "live_trading_confirmed",
         "backtest_date", "data_output_path", "data_output_max_mb",
         "historical_training_sessions",
         "historical_validation_sessions"},
        "run");
    std::string runMode = "paper";
    readValue(run, "mode", runMode, "run");
    config.run.mode = parseRunMode(runMode);
    readValue(run, "force_login", config.run.forceLogin, "run");
    readValue(run, "live_trading_confirmed",
              config.run.liveTradingConfirmed, "run");
    readValue(run, "backtest_date", config.run.backtestDate, "run");
    readValue(run, "data_output_path", config.run.dataOutputPath, "run");
    readUnsignedValue(run, "data_output_max_mb",
                      config.run.dataOutputMaxMb, "run");
    readUnsignedValue(run, "historical_training_sessions",
                      config.run.historicalTrainingSessions, "run");
    readUnsignedValue(run, "historical_validation_sessions",
                      config.run.historicalValidationSessions, "run");
    if (config.run.mode == RunMode::Backtest) {
        validateBacktestDate(config.run.backtestDate);
    }
    if (config.run.mode == RunMode::Live &&
        !config.run.liveTradingConfirmed) {
        throw std::invalid_argument(
            "run.live_trading_confirmed must be true for live mode");
    }
    if (config.run.mode == RunMode::ExportBacktestData &&
        (config.run.historicalTrainingSessions == 0 ||
         config.run.historicalValidationSessions == 0)) {
        throw std::invalid_argument(
            "Historical training and validation session counts must be positive");
    }
    if (config.run.dataOutputPath.empty()) {
        throw std::invalid_argument("run.data_output_path must not be empty");
    }
    if (config.run.dataOutputMaxMb == 0) {
        throw std::invalid_argument("run.data_output_max_mb must be positive");
    }
    readValue(root, "allow_short_selling", config.allowShortSelling, "root");

    const Json& marketData = objectSection(root, "market_data");
    rejectUnknownKeys(
        marketData,
        {"quote_poll_interval_ms", "quote_max_age_seconds",
         "fallback_tick_size"},
        "market_data");
    int quotePollMilliseconds =
        static_cast<int>(config.quotePollInterval.count());
    int quoteMaxAgeSeconds = 5;
    readIntValue(marketData, "quote_poll_interval_ms",
                 quotePollMilliseconds, "market_data");
    readIntValue(marketData, "quote_max_age_seconds",
                 quoteMaxAgeSeconds, "market_data");
    readValue(marketData, "fallback_tick_size",
              config.fallbackTickSize, "market_data");
    config.quotePollInterval =
        std::chrono::milliseconds(quotePollMilliseconds);

    const Json& selection = objectSection(root, "selection");
    rejectUnknownKeys(
        selection,
        {"min_price", "require_nifty100", "min_relative_volume",
         "min_absolute_volume", "max_spread_percent", "min_depth_quantity",
         "min_price_change_percent", "max_price_change_percent",
         "momentum_lookback_seconds", "trend_lookback_seconds",
         "min_trend_change_percent",
         "min_intraday_range_percent", "volume_ewma_alpha",
         "summary_interval_seconds", "worker_count"},
        "selection");
    readValue(selection, "min_price", config.selection.minPrice, "selection");
    readValue(selection, "require_nifty100",
              config.selection.requireNifty100, "selection");
    readValue(selection, "min_relative_volume",
              config.selection.minRelativeVolume, "selection");
    readUnsignedValue(selection, "min_absolute_volume",
                      config.selection.minAbsoluteVolume, "selection");
    readValue(selection, "max_spread_percent",
              config.selection.maxSpreadPercent, "selection");
    readValue(selection, "min_depth_quantity",
              config.selection.minDepthQuantity, "selection");
    readValue(selection, "min_price_change_percent",
              config.selection.minPriceChangePercent, "selection");
    readUnsignedValue(selection, "momentum_lookback_seconds",
                      config.selection.momentumLookbackSeconds, "selection");
    readUnsignedValue(selection, "trend_lookback_seconds",
                      config.selection.trendLookbackSeconds, "selection");
    readValue(selection, "min_trend_change_percent",
              config.selection.minTrendChangePercent, "selection");
    readValue(selection, "max_price_change_percent",
              config.selection.maxPriceChangePercent, "selection");
    readValue(selection, "min_intraday_range_percent",
              config.selection.minIntradayRangePercent, "selection");
    readValue(selection, "volume_ewma_alpha",
              config.selection.volumeEwmaAlpha, "selection");
    int selectionSummarySeconds =
        static_cast<int>(config.selection.summaryInterval.count());
    readIntValue(selection, "summary_interval_seconds",
                 selectionSummarySeconds, "selection");
    config.selection.summaryInterval =
        std::chrono::seconds(selectionSummarySeconds);
    readUnsignedValue(selection, "worker_count",
                      config.selection.workerCount, "selection");

    const Json& decision = objectSection(root, "decision");
    rejectUnknownKeys(
        decision,
        {"minimum_imbalance", "maximum_spread_percent", "target_percent",
         "stop_percent", "require_volume_confirmation",
         "min_relative_volume", "indicative_entry_budget",
         "imbalance_level_weights", "entry_trigger_offset_ticks",
         "entry_limit_offset_ticks", "required_confirmations",
         "confirmation_interval_ms", "worker_count"},
        "decision");
    readValue(decision, "minimum_imbalance",
              config.decision.minimumImbalance, "decision");
    readValue(decision, "maximum_spread_percent",
              config.decision.maximumSpreadPercent, "decision");
    readValue(decision, "target_percent",
              config.decision.targetPercent, "decision");
    readValue(decision, "stop_percent",
              config.decision.stopPercent, "decision");
    readValue(decision, "require_volume_confirmation",
              config.decision.requireVolumeConfirmation, "decision");
    readValue(decision, "min_relative_volume",
              config.decision.minRelativeVolume, "decision");
    readValue(decision, "indicative_entry_budget",
              config.decision.indicativeEntryBudget, "decision");
    const auto weights = decision.find("imbalance_level_weights");
    if (weights != decision.end()) {
        if (!weights->is_array() ||
            weights->size() != config.decision.imbalanceLevelWeights.size()) {
            throw std::invalid_argument(
                "decision.imbalance_level_weights must contain five values");
        }
        try {
            for (std::size_t index = 0;
                 index < config.decision.imbalanceLevelWeights.size(); ++index) {
                if (!weights->at(index).is_number()) {
                    throw std::invalid_argument(
                        "imbalance weights must be numeric");
                }
                config.decision.imbalanceLevelWeights[index] =
                    weights->at(index).get<double>();
            }
        } catch (const Json::exception& exception) {
            throw std::invalid_argument(
                std::string("Invalid configuration value for "
                            "decision.imbalance_level_weights: ") +
                exception.what());
        } catch (const std::invalid_argument&) {
            throw std::invalid_argument(
                "decision.imbalance_level_weights must contain numbers");
        }
    }
    readUnsignedValue(decision, "entry_trigger_offset_ticks",
                      config.decision.entryTriggerOffsetTicks, "decision");
    readUnsignedValue(decision, "entry_limit_offset_ticks",
                      config.decision.entryLimitOffsetTicks, "decision");
    readUnsignedValue(decision, "required_confirmations",
                      config.decision.requiredConfirmations, "decision");
    int confirmationIntervalMilliseconds =
        static_cast<int>(config.decision.pollInterval.count());
    readIntValue(decision, "confirmation_interval_ms",
                 confirmationIntervalMilliseconds, "decision");
    config.decision.pollInterval =
        std::chrono::milliseconds(confirmationIntervalMilliseconds);
    readUnsignedValue(decision, "worker_count",
                      config.decision.workerCount, "decision");

    const Json& handling = objectSection(root, "handling");
    rejectUnknownKeys(
        handling,
        {"poll_interval_ms", "entry_fill_timeout_seconds",
         "exit_fill_timeout_seconds", "max_holding_seconds",
         "reversal_imbalance_threshold", "reversal_confirmations",
         "worker_count"},
        "handling");
    int handlingPollMilliseconds =
        static_cast<int>(config.handling.pollInterval.count());
    int entryTimeoutSeconds =
        static_cast<int>(config.handling.entryFillTimeout.count());
    int exitTimeoutSeconds =
        static_cast<int>(config.handling.exitFillTimeout.count());
    int maxHoldingSeconds =
        static_cast<int>(config.handling.maxHoldingTime.count());
    readIntValue(handling, "poll_interval_ms",
                 handlingPollMilliseconds, "handling");
    readIntValue(handling, "entry_fill_timeout_seconds",
                 entryTimeoutSeconds, "handling");
    readIntValue(handling, "exit_fill_timeout_seconds",
                 exitTimeoutSeconds, "handling");
    readIntValue(handling, "max_holding_seconds",
                 maxHoldingSeconds, "handling");
    readValue(handling, "reversal_imbalance_threshold",
              config.handling.reversalImbalanceThreshold, "handling");
    readUnsignedValue(handling, "reversal_confirmations",
                      config.handling.reversalConfirmations, "handling");
    readUnsignedValue(handling, "worker_count",
                      config.handling.workerCount, "handling");
    config.handling.pollInterval =
        std::chrono::milliseconds(handlingPollMilliseconds);
    config.handling.entryFillTimeout =
        std::chrono::seconds(entryTimeoutSeconds);
    config.handling.exitFillTimeout =
        std::chrono::seconds(exitTimeoutSeconds);
    config.handling.maxHoldingTime =
        std::chrono::seconds(maxHoldingSeconds);

    const Json& risk = objectSection(root, "risk");
    rejectUnknownKeys(
        risk, {"max_open_positions", "paper_trading_cash"}, "risk");
    readUnsignedValue(risk, "max_open_positions",
                      config.maxOpenPositions, "risk");
    readValue(risk, "paper_trading_cash",
              config.paperTradingCash, "risk");

    const Json& backtest = objectSection(root, "backtest");
    rejectUnknownKeys(
        backtest,
        {"initial_capital", "max_open_positions",
         "entry_timeout_minutes", "entry_timeout_seconds",
         "entry_start_time", "entry_end_time",
         "synthesize_one_second_quotes", "synthetic_base_spread_percent",
         "synthetic_volatility_spread_factor",
         "synthetic_maximum_spread_percent", "max_holding_seconds",
         "brokerage_rate_percent", "brokerage_cap_per_order",
         "stt_sell_rate_percent", "exchange_rate_percent",
         "sebi_rate_percent", "stamp_buy_rate_percent", "gst_percent"},
        "backtest");
    readValue(backtest, "initial_capital",
              config.backtest.initialCapital, "backtest");
    readUnsignedValue(backtest, "max_open_positions",
                      config.backtest.maxOpenPositions, "backtest");
    readIntValue(backtest, "entry_timeout_minutes",
                 config.backtest.entryTimeoutMinutes, "backtest");
    readIntValue(backtest, "entry_timeout_seconds",
                 config.backtest.entryTimeoutSeconds, "backtest");
    readValue(backtest, "entry_start_time",
              config.backtest.entryStartTime, "backtest");
    readValue(backtest, "entry_end_time",
              config.backtest.entryEndTime, "backtest");
    readValue(backtest, "synthesize_one_second_quotes",
              config.backtest.synthesizeOneSecondQuotes, "backtest");
    readValue(backtest, "synthetic_base_spread_percent",
              config.backtest.syntheticBaseSpreadPercent, "backtest");
    readValue(backtest, "synthetic_volatility_spread_factor",
              config.backtest.syntheticVolatilitySpreadFactor, "backtest");
    readValue(backtest, "synthetic_maximum_spread_percent",
              config.backtest.syntheticMaximumSpreadPercent, "backtest");
    readIntValue(backtest, "max_holding_seconds",
                 config.backtest.maxHoldingSeconds, "backtest");
    readValue(backtest, "brokerage_rate_percent",
              config.backtest.brokerageRatePercent, "backtest");
    readValue(backtest, "brokerage_cap_per_order",
              config.backtest.brokerageCapPerOrder, "backtest");
    readValue(backtest, "stt_sell_rate_percent",
              config.backtest.sellSideSttPercent, "backtest");
    readValue(backtest, "exchange_rate_percent",
              config.backtest.transactionChargePercent, "backtest");
    readValue(backtest, "sebi_rate_percent",
              config.backtest.sebiChargePercent, "backtest");
    readValue(backtest, "stamp_buy_rate_percent",
              config.backtest.buySideStampPercent, "backtest");
    readValue(backtest, "gst_percent",
              config.backtest.gstPercent, "backtest");

    requireFinitePositive(config.selection.minPrice, "selection.min_price");
    requireFiniteNonnegative(
        config.selection.minRelativeVolume,
        "selection.min_relative_volume");
    requireFiniteNonnegative(
        config.selection.maxSpreadPercent,
        "selection.max_spread_percent");
    requireFiniteNonnegative(
        config.selection.minDepthQuantity,
        "selection.min_depth_quantity");
    requireFiniteNonnegative(
        config.selection.minPriceChangePercent,
        "selection.min_price_change_percent");
    requireFiniteNonnegative(
        config.selection.minTrendChangePercent,
        "selection.min_trend_change_percent");
    requireFinitePositive(
        config.selection.maxPriceChangePercent,
        "selection.max_price_change_percent");
    requireFiniteNonnegative(
        config.selection.minIntradayRangePercent,
        "selection.min_intraday_range_percent");
    if (config.selection.minPriceChangePercent >
        config.selection.maxPriceChangePercent) {
        throw std::invalid_argument(
            "Selection minimum price change exceeds its maximum");
    }
    if (config.selection.trendLookbackSeconds > 0 &&
        config.selection.momentumLookbackSeconds > 0 &&
        config.selection.trendLookbackSeconds <=
            config.selection.momentumLookbackSeconds) {
        throw std::invalid_argument(
            "Selection trend lookback must exceed momentum lookback");
    }
    if (selectionSummarySeconds <= 0 ||
        config.selection.workerCount == 0) {
        throw std::invalid_argument(
            "Selection interval and worker count must be positive");
    }
    requireFinitePositive(
        config.selection.volumeEwmaAlpha,
        "selection.volume_ewma_alpha");
    if (config.selection.volumeEwmaAlpha > 1.0) {
        throw std::invalid_argument(
            "selection.volume_ewma_alpha must not exceed 1");
    }

    requireFiniteNonnegative(
        config.decision.minimumImbalance,
        "decision.minimum_imbalance");
    requireFiniteNonnegative(
        config.decision.maximumSpreadPercent,
        "decision.maximum_spread_percent");
    requireFinitePositive(
        config.decision.targetPercent, "decision.target_percent");
    requireFinitePositive(
        config.decision.stopPercent, "decision.stop_percent");
    requireFinitePositive(
        config.decision.minRelativeVolume,
        "decision.min_relative_volume");
    requireFinitePositive(
        config.decision.indicativeEntryBudget,
        "decision.indicative_entry_budget");
    double totalImbalanceWeight = 0.0;
    for (double weight : config.decision.imbalanceLevelWeights) {
        requireFiniteNonnegative(
            weight, "decision.imbalance_level_weights");
        totalImbalanceWeight += weight;
    }
    if (config.decision.targetPercent >= 100.0 ||
        config.decision.stopPercent >= 100.0 ||
        config.decision.minimumImbalance > 1.0 ||
        totalImbalanceWeight <= 0.0 ||
        config.decision.workerCount == 0 ||
        config.decision.requiredConfirmations == 0 ||
        confirmationIntervalMilliseconds <= 0) {
        throw std::invalid_argument(
            "Decision thresholds, weights, interval, or worker count are invalid");
    }

    if (handlingPollMilliseconds <= 0 ||
        entryTimeoutSeconds <= 0 ||
        exitTimeoutSeconds <= 0 ||
        maxHoldingSeconds < 0 ||
        config.handling.reversalConfirmations == 0 ||
        config.handling.workerCount == 0) {
        throw std::invalid_argument(
            "Handling intervals, timeouts, and worker count must be positive");
    }
    requireFiniteNonnegative(
        config.handling.reversalImbalanceThreshold,
        "handling.reversal_imbalance_threshold");
    if (config.handling.reversalImbalanceThreshold > 1.0) {
        throw std::invalid_argument(
            "handling.reversal_imbalance_threshold must not exceed 1");
    }
    if (config.maxOpenPositions == 0) {
        throw std::invalid_argument(
            "risk.max_open_positions must be positive");
    }
    requireFinitePositive(config.paperTradingCash, "risk.paper_trading_cash");
    requireFinitePositive(
        config.fallbackTickSize, "market_data.fallback_tick_size");
    if (quotePollMilliseconds <= 0 || quoteMaxAgeSeconds <= 0) {
        throw std::invalid_argument(
            "Market-data polling interval and quote age must be positive");
    }

    requireFinitePositive(
        config.backtest.initialCapital, "backtest.initial_capital");
    if (config.backtest.maxOpenPositions == 0) {
        throw std::invalid_argument(
            "backtest.max_open_positions must be positive");
    }
    if (config.backtest.entryTimeoutMinutes <= 0 ||
        config.backtest.entryTimeoutSeconds < 0) {
        throw std::invalid_argument(
            "Backtest entry timeout values are invalid");
    }
    validateClockTime(
        config.backtest.entryStartTime, "backtest.entry_start_time");
    validateClockTime(
        config.backtest.entryEndTime, "backtest.entry_end_time");
    if (config.backtest.entryStartTime >= config.backtest.entryEndTime) {
        throw std::invalid_argument(
            "Backtest entry window must start before it ends");
    }
    requireFiniteNonnegative(
        config.backtest.syntheticBaseSpreadPercent,
        "backtest.synthetic_base_spread_percent");
    requireFiniteNonnegative(
        config.backtest.syntheticVolatilitySpreadFactor,
        "backtest.synthetic_volatility_spread_factor");
    requireFiniteNonnegative(
        config.backtest.syntheticMaximumSpreadPercent,
        "backtest.synthetic_maximum_spread_percent");
    if (config.backtest.syntheticBaseSpreadPercent >
        config.backtest.syntheticMaximumSpreadPercent) {
        throw std::invalid_argument(
            "Synthetic base spread must not exceed its maximum");
    }
    if (config.backtest.maxHoldingSeconds < 0) {
        throw std::invalid_argument(
            "backtest.max_holding_seconds must be nonnegative");
    }
    requireFiniteNonnegative(
        config.backtest.brokerageRatePercent,
        "backtest.brokerage_rate_percent");
    requireFiniteNonnegative(
        config.backtest.brokerageCapPerOrder,
        "backtest.brokerage_cap_per_order");
    requireFiniteNonnegative(
        config.backtest.sellSideSttPercent,
        "backtest.stt_sell_rate_percent");
    requireFiniteNonnegative(
        config.backtest.transactionChargePercent,
        "backtest.exchange_rate_percent");
    requireFiniteNonnegative(
        config.backtest.sebiChargePercent,
        "backtest.sebi_rate_percent");
    requireFiniteNonnegative(
        config.backtest.buySideStampPercent,
        "backtest.stamp_buy_rate_percent");
    requireFiniteNonnegative(
        config.backtest.gstPercent, "backtest.gst_percent");

    config.selection.allowShortSelling = config.allowShortSelling;
    config.selection.maxQuoteAge = std::chrono::seconds(quoteMaxAgeSeconds);
    config.decision.allowShortSelling = config.allowShortSelling;
    config.decision.maxQuoteAge = std::chrono::seconds(quoteMaxAgeSeconds);
    config.decision.fallbackTickSize = config.fallbackTickSize;
    config.handling.allowShortSelling = config.allowShortSelling;
    config.handling.maxQuoteAge = std::chrono::seconds(quoteMaxAgeSeconds);
    config.handling.paperTradingCash = config.paperTradingCash;
    config.handling.imbalanceLevelWeights =
        config.decision.imbalanceLevelWeights;
    config.backtest.fallbackTickSize = config.fallbackTickSize;
    if (config.backtest.maxHoldingSeconds == 0) {
        config.backtest.maxHoldingSeconds = maxHoldingSeconds;
    }
    config.backtest.selection = config.selection;
    config.backtest.decision = config.decision;
    return config;
}

RuntimeConfig loadRuntimeConfig(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error(
            "Unable to open configuration file: " + path);
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        throw std::runtime_error(
            "Failed while reading configuration file: " + path);
    }
    try {
        return parseRuntimeConfig(contents.str());
    } catch (const std::exception& exception) {
        throw std::runtime_error(
            "Invalid configuration file " + path + ": " + exception.what());
    }
}

} // namespace zerotick
