#include "zerotick/KiteApi.h"
#include "zerotick/BacktestEngine.h"
#include "zerotick/Log.h"
#include "zerotick/EngineTypes.h"
#include "zerotick/StockSelectionEngine.h"
#include "zerotick/TradingDecisionEngine.h"
#include "zerotick/TradeHandlingEngine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cmath>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic_bool shutdownRequested{false};

void handleSignal(int) {
    shutdownRequested.store(true);
}

void refreshQuotesInChunks(zerotick::KiteApi& api,
                           const std::vector<std::string>& symbols,
                           std::size_t chunkSize,
                           zerotick::ThreadSafeQueue<
                               zerotick::MarketSnapshot>& quoteUpdates) {
    for (std::size_t index = 0; index < symbols.size(); index += chunkSize) {
        std::vector<std::string> chunk;
        chunk.reserve(chunkSize);
        for (std::size_t j = index; j < index + chunkSize && j < symbols.size();
             ++j) {
            chunk.push_back(symbols[j]);
        }
        const std::vector<zerotick::MarketSnapshot> refreshed =
            api.refreshQuotes(chunk);
        for (const zerotick::MarketSnapshot& snapshot : refreshed) {
            quoteUpdates.push(snapshot);
        }
    }
}

std::map<std::string, std::vector<zerotick::HistoricalCandle>>
downloadBacktestCandles(
    const zerotick::KiteConnectApi& api,
    const std::vector<std::string>& symbols,
    const std::string& date,
    std::map<std::string, double>& tickSizes,
    std::size_t& symbolsWithoutData) {
    std::map<std::string, std::vector<zerotick::HistoricalCandle>> data;
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        if (index > 0) {
            // Stay below Kite's historical endpoint request-rate limit.
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }

        const std::string& symbol = symbols[index];
        const std::vector<zerotick::HistoricalCandle> candles =
            api.getHistoricalCandles(symbol, date);
        if (candles.empty()) {
            ++symbolsWithoutData;
            zerotick::logLine(
                "[backtest] no_candles symbol=" + symbol +
                " date=" + date);
        } else {
            data[symbol] = candles;
            tickSizes[symbol] = api.getInstrumentTickSize(symbol);
        }

        if ((index + 1) % 10 == 0 || index + 1 == symbols.size()) {
            zerotick::logLine(
                "[backtest] instruments_processed=" +
                std::to_string(index + 1) + "/" +
                std::to_string(symbols.size()));
        }
    }
    return data;
}

void printBacktestSummary(
    const std::string& date,
    std::size_t symbolsWithoutData,
    const zerotick::BacktestResult& result) {
    std::size_t winningTrades = 0;
    for (const zerotick::BacktestTrade& trade : result.trades) {
        if (trade.grossProfitLoss > 0.0) {
            ++winningTrades;
        }
    }
    const double winRate = result.trades.empty()
        ? 0.0
        : static_cast<double>(winningTrades) /
              static_cast<double>(result.trades.size()) * 100.0;
    const double returnPercent =
        result.grossProfitLoss / result.initialCapital * 100.0;

    const auto sessionTime = [](const std::string& timestamp) {
        if (timestamp.size() >= 16 && timestamp[10] == 'T') {
            return timestamp.substr(11, 5);
        }
        return timestamp;
    };

    std::cout << std::fixed << std::setprecision(2)
              << "Backtest date: " << date << " (Kite 1-minute candles)\n"
              << "\nClosed trades (times shown as HH:MM):\n"
              << std::left
              << std::setw(4) << "#"
              << std::setw(13) << "Symbol"
              << std::setw(8) << "Signal"
              << std::setw(8) << "Entry"
              << std::setw(8) << "Exit"
              << std::right
              << std::setw(6) << "Qty"
              << std::setw(12) << "Buy price"
              << std::setw(12) << "Sell price"
              << std::setw(12) << "Gross P&L"
              << "  " << std::left << "Exit reason" << '\n'
              << std::string(96, '-') << '\n';

    if (result.trades.empty()) {
        std::cout << "No closed trades.\n";
    }
    for (std::size_t index = 0; index < result.trades.size(); ++index) {
        const zerotick::BacktestTrade& trade = result.trades[index];
        std::cout << std::left
                  << std::setw(4) << index + 1
                  << std::setw(13) << trade.symbol
                  << std::setw(8) << sessionTime(trade.signalTime)
                  << std::setw(8) << sessionTime(trade.entryTime)
                  << std::setw(8) << sessionTime(trade.exitTime)
                  << std::right
                  << std::setw(6) << trade.quantity
                  << std::setw(12) << trade.entryPrice
                  << std::setw(12) << trade.exitPrice
                  << std::setw(12) << std::showpos
                  << trade.grossProfitLoss << std::noshowpos
                  << "  " << std::left << trade.exitReason << '\n';
    }

    std::cout << "\nStarting capital: INR " << result.initialCapital << "\n"
              << "Ending capital: INR " << result.endingCapital << "\n"
              << "Gross return: " << returnPercent << "%\n"
              << "Max drawdown: " << result.maxDrawdownPercent << "%\n"
              << "Signals: " << result.generatedSignals
              << ", filled entries: " << result.filledEntries
              << ", unfilled entries: " << result.unfilledEntries
              << ", risk rejected: " << result.riskRejectedSignals << "\n"
              << "Closed trades: " << result.trades.size()
              << ", winners: " << winningTrades
              << ", win rate: " << winRate << "%\n"
              << "Symbols without candle data: " << symbolsWithoutData << "\n"
              << "Approximation: spread/depth/imbalance filters, fees, and "
                 "slippage are not modeled.\n"
              << "TOTAL PROFIT / LOSS: INR " << std::showpos
              << result.grossProfitLoss << std::noshowpos << '\n';
}

struct SegmentStats {
    std::size_t trades = 0;
    std::size_t winners = 0;
    double grossProfit = 0.0;
    double grossLoss = 0.0;
};

struct OptimizationVariant {
    zerotick::BacktestConfig config;
    bool currentDefaults = false;
    double initialCapital = 0.0;
    double capital = 0.0;
    double trainingEndingCapital = 0.0;
    double holdoutPeakCapital = 0.0;
    double maximumHoldoutCloseDrawdownPercent = 0.0;
    SegmentStats training;
    SegmentStats holdout;
};

std::string addCalendarDays(const std::string& date, int days) {
    std::tm value = {};
    value.tm_year = std::stoi(date.substr(0, 4)) - 1900;
    value.tm_mon = std::stoi(date.substr(5, 2)) - 1;
    value.tm_mday = std::stoi(date.substr(8, 2)) + days;
    value.tm_hour = 12;
    if (std::mktime(&value) == static_cast<std::time_t>(-1)) {
        throw std::runtime_error("Unable to calculate backtest date range");
    }

    std::ostringstream formatted;
    formatted << std::setfill('0')
              << std::setw(4) << value.tm_year + 1900 << '-'
              << std::setw(2) << value.tm_mon + 1 << '-'
              << std::setw(2) << value.tm_mday;
    return formatted.str();
}

std::string latestCompletedDate() {
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    if (localtime_s(&local, &now) != 0) {
        throw std::runtime_error("Unable to read local date for backtest");
    }

    std::ostringstream formatted;
    formatted << std::setfill('0')
              << std::setw(4) << local.tm_year + 1900 << '-'
              << std::setw(2) << local.tm_mon + 1 << '-'
              << std::setw(2) << local.tm_mday;
    const std::string today = formatted.str();
    const int minutesSinceMidnight = local.tm_hour * 60 + local.tm_min;
    return minutesSinceMidnight >= 15 * 60 + 35
        ? today
        : addCalendarDays(today, -1);
}

std::vector<std::string> latestTradingSessions(
    const zerotick::KiteConnectApi& api,
    const zerotick::NiftyUniverse& universe,
    std::size_t sessionCount) {
    const std::vector<std::string> symbols = universe.symbols();
    if (symbols.empty()) {
        throw std::runtime_error("The configured stock universe is empty");
    }

    std::set<std::string> sessions;
    std::string chunkEnd = latestCompletedDate();
    for (std::size_t chunk = 0;
         chunk < 40 && sessions.size() < sessionCount;
         ++chunk) {
        const std::string chunkStart = addCalendarDays(chunkEnd, -6);
        if (chunk > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
        const std::vector<zerotick::HistoricalCandle> candles =
            api.getHistoricalCandles(symbols.front(), chunkStart, chunkEnd);
        for (const zerotick::HistoricalCandle& candle : candles) {
            if (candle.timestamp.size() >= 10) {
                sessions.insert(candle.timestamp.substr(0, 10));
            }
        }
        chunkEnd = addCalendarDays(chunkStart, -1);
    }

    if (sessions.size() < sessionCount) {
        throw std::runtime_error(
            "Kite returned fewer than " + std::to_string(sessionCount) +
            " trading sessions in the available lookback");
    }

    std::vector<std::string> dates(sessions.begin(), sessions.end());
    dates.erase(dates.begin(),
                dates.begin() + (dates.size() - sessionCount));
    return dates;
}

std::vector<OptimizationVariant> makeOptimizationVariants(
    const zerotick::BacktestConfig& baseConfig) {
    struct SelectionProfile {
        double relativeVolume;
        double priceChange;
        double intradayRange;
    };
    struct ExitProfile {
        double target;
        double stop;
    };

    const SelectionProfile selectionProfiles[] = {
        {1.5, 0.05, 0.30},
        {1.8, 0.05, 0.30},
        {1.5, 0.10, 0.30},
        {1.5, 0.05, 0.50},
        {2.0, 0.10, 0.50}
    };
    const ExitProfile exitProfiles[] = {
        {0.20, 0.40},
        {0.30, 0.20},
        {0.40, 0.20},
        {0.40, 0.30}
    };

    std::vector<OptimizationVariant> variants;
    for (const SelectionProfile& selection : selectionProfiles) {
        for (const ExitProfile& exits : exitProfiles) {
            OptimizationVariant variant;
            variant.config = baseConfig;
            variant.config.selection.minRelativeVolume =
                selection.relativeVolume;
            variant.config.selection.minPriceChangePercent =
                selection.priceChange;
            variant.config.selection.minIntradayRangePercent =
                selection.intradayRange;
            variant.config.decision.targetPercent = exits.target;
            variant.config.decision.stopPercent = exits.stop;
            variant.currentDefaults =
                selection.relativeVolume == 1.5 &&
                selection.priceChange == 0.05 &&
                selection.intradayRange == 0.30 &&
                exits.target == 0.40 && exits.stop == 0.20;
            variant.initialCapital = baseConfig.initialCapital;
            variant.capital = baseConfig.initialCapital;
            variants.push_back(variant);
        }
    }
    return variants;
}

void addTradeStats(SegmentStats& stats,
                   const std::vector<zerotick::BacktestTrade>& trades) {
    for (const zerotick::BacktestTrade& trade : trades) {
        ++stats.trades;
        if (trade.grossProfitLoss > 0.0) {
            ++stats.winners;
            stats.grossProfit += trade.grossProfitLoss;
        } else if (trade.grossProfitLoss < 0.0) {
            stats.grossLoss += -trade.grossProfitLoss;
        }
    }
}

double winRatePercent(const SegmentStats& stats) {
    return stats.trades == 0
        ? 0.0
        : static_cast<double>(stats.winners) /
              static_cast<double>(stats.trades) * 100.0;
}

double profitFactor(const SegmentStats& stats) {
    if (stats.grossLoss == 0.0) {
        return stats.grossProfit > 0.0
            ? std::numeric_limits<double>::infinity()
            : 0.0;
    }
    return stats.grossProfit / stats.grossLoss;
}

void printOptimizationResults(
    const std::vector<OptimizationVariant>& variants,
    std::size_t trainingDays,
    std::size_t holdoutDays) {
    std::vector<std::size_t> ranking;
    for (std::size_t index = 0; index < variants.size(); ++index) {
        ranking.push_back(index);
    }
    std::sort(ranking.begin(), ranking.end(),
              [&variants](std::size_t left, std::size_t right) {
                  return variants[left].trainingEndingCapital >
                         variants[right].trainingEndingCapital;
              });

    std::cout << std::fixed << std::setprecision(2)
              << "\n100-session parameter sweep (ranked by training P&L)\n"
              << "Training sessions: " << trainingDays
              << ", untouched holdout sessions: " << holdoutDays << '\n'
              << std::left
              << std::setw(5) << "Rank"
              << " | "
              << std::setw(7) << "RelVol"
              << " | "
              << std::setw(8) << "Move%"
              << " | "
              << std::setw(8) << "Range%"
              << " | "
              << std::setw(8) << "Target"
              << " | "
              << std::setw(7) << "Stop"
              << " | "
              << std::setw(14) << "Train P&L"
              << " | "
              << std::setw(14) << "Holdout P&L"
              << " | "
              << std::setw(8) << "PF"
              << " | "
              << std::setw(9) << "Win%"
              << " | "
              << std::setw(8) << "Trades"
              << " | "
              << std::setw(10) << "Hold DD"
              << " | " << std::setw(7) << "Mark" << '\n'
              << std::string(149, '-') << '\n';

    for (std::size_t rank = 0; rank < ranking.size(); ++rank) {
        const OptimizationVariant& variant = variants[ranking[rank]];
        const double trainPnl =
            variant.trainingEndingCapital - variant.initialCapital;
        const double holdoutPnl =
            variant.capital - variant.trainingEndingCapital;
        const double holdoutProfitFactor = profitFactor(variant.holdout);
        std::ostringstream factor;
        if (std::isinf(holdoutProfitFactor)) {
            factor << "inf";
        } else {
            factor << std::fixed << std::setprecision(2)
                   << holdoutProfitFactor;
        }

        std::cout << std::left
                  << std::setw(5) << rank + 1
                  << " | "
                  << std::setw(7)
                  << variant.config.selection.minRelativeVolume
                  << " | "
                  << std::setw(8)
                  << variant.config.selection.minPriceChangePercent
                  << " | "
                  << std::setw(8)
                  << variant.config.selection.minIntradayRangePercent
                  << " | "
                  << std::setw(8) << variant.config.decision.targetPercent
                  << " | "
                  << std::setw(7) << variant.config.decision.stopPercent
                  << std::right << std::showpos
                  << " | "
                  << std::setw(14) << trainPnl
                  << " | "
                  << std::setw(14) << holdoutPnl
                  << std::noshowpos << std::left
                  << " | "
                  << std::setw(8) << factor.str()
                  << " | "
                  << std::setw(9) << winRatePercent(variant.holdout)
                  << " | "
                  << std::setw(8) << variant.holdout.trades
                  << " | "
                  << std::setw(10)
                  << variant.maximumHoldoutCloseDrawdownPercent
                  << " | "
                  << std::setw(7)
                  << (variant.currentDefaults ? "current" : "") << '\n';
    }

    const OptimizationVariant& trainingBest = variants[ranking.front()];
    const double bestHoldoutPnl =
        trainingBest.capital - trainingBest.trainingEndingCapital;
    const double bestTotalPnl =
        trainingBest.capital - trainingBest.initialCapital;
    const auto current = std::find_if(
        variants.begin(), variants.end(),
        [](const OptimizationVariant& variant) {
            return variant.currentDefaults;
        });
    std::cout << "\nTraining-selected candidate: target "
              << trainingBest.config.decision.targetPercent
              << "%, stop " << trainingBest.config.decision.stopPercent
              << "%, relative volume "
              << trainingBest.config.selection.minRelativeVolume
              << ", min price change "
              << trainingBest.config.selection.minPriceChangePercent
              << "%, min range "
              << trainingBest.config.selection.minIntradayRangePercent
              << "%; holdout P&L INR " << std::showpos << bestHoldoutPnl
              << std::noshowpos << ", 100-session P&L INR "
              << std::showpos << bestTotalPnl << std::noshowpos << ".\n";
    if (current != variants.end()) {
        const double currentHoldoutPnl =
            current->capital - current->trainingEndingCapital;
        const double currentTotalPnl =
            current->capital - current->initialCapital;
        std::cout << "Current defaults: holdout P&L INR "
                  << std::showpos << currentHoldoutPnl
                  << std::noshowpos << ", 100-session P&L INR "
                  << std::showpos << currentTotalPnl
                  << std::noshowpos << ".\n";
    }
    if (bestHoldoutPnl <= 0.0) {
        std::cout << "The training-selected candidate was not gross "
                     "profitable on holdout; this sweep provides no basis "
                     "for claiming profitability.\n";
    }
    std::cout << "Results are gross: fees, taxes, and slippage are excluded. "
                 "Holdout drawdown is based on daily closing equity.\n"
              << "TRAIN-SELECTED TOTAL P&L (100 sessions): INR "
              << std::showpos << bestTotalPnl << std::noshowpos << '\n';
}

void runOptimizationStudy(
    const zerotick::KiteConnectApi& api,
    const zerotick::NiftyUniverse& universe,
    const zerotick::BacktestConfig& baseConfig) {
    constexpr std::size_t sessionCount = 100;
    constexpr std::size_t trainingDays = 70;
    const std::size_t holdoutDays = sessionCount - trainingDays;
    const std::vector<std::string> sessions =
        latestTradingSessions(api, universe, sessionCount);
    const std::vector<std::string> symbols = universe.symbols();
    std::vector<OptimizationVariant> variants =
        makeOptimizationVariants(baseConfig);
    zerotick::BacktestEngine engine;
    std::set<std::string> symbolsWithData;
    std::size_t nextSession = 0;

    zerotick::logLine(
        "[optimization] sessions=" + std::to_string(sessionCount) +
        " first=" + sessions.front() + " last=" + sessions.back() +
        " variants=" + std::to_string(variants.size()) +
        " train_days=" + std::to_string(trainingDays) +
        " holdout_days=" + std::to_string(holdoutDays));

    while (nextSession < sessions.size()) {
        const std::string chunkStart = sessions[nextSession];
        const std::string chunkEndCandidate =
            addCalendarDays(chunkStart, 6);
        const std::string chunkEnd =
            chunkEndCandidate < sessions.back()
                ? chunkEndCandidate
                : sessions.back();
        std::map<std::string, std::vector<zerotick::HistoricalCandle>>
            chunkCandles;
        std::map<std::string, double> tickSizes;

        zerotick::logLine(
            "[optimization] fetching_range=" + chunkStart + ".." +
            chunkEnd);
        for (std::size_t index = 0; index < symbols.size(); ++index) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const std::string& symbol = symbols[index];
            std::vector<zerotick::HistoricalCandle> candles =
                api.getHistoricalCandles(symbol, chunkStart, chunkEnd);
            if (!candles.empty()) {
                symbolsWithData.insert(symbol);
                tickSizes[symbol] = api.getInstrumentTickSize(symbol);
                chunkCandles[symbol] = std::move(candles);
            }
            if ((index + 1) % 10 == 0 || index + 1 == symbols.size()) {
                zerotick::logLine(
                    "[optimization] range_symbols=" +
                    std::to_string(index + 1) + "/" +
                    std::to_string(symbols.size()));
            }
        }

        while (nextSession < sessions.size() &&
               sessions[nextSession] <= chunkEnd) {
            const std::string& date = sessions[nextSession];
            std::map<std::string, std::vector<zerotick::HistoricalCandle>>
                dayCandles;
            for (const auto& symbolData : chunkCandles) {
                for (const zerotick::HistoricalCandle& candle :
                     symbolData.second) {
                    if (candle.timestamp.compare(0, 10, date) == 0) {
                        dayCandles[symbolData.first].push_back(candle);
                    }
                }
            }
            if (dayCandles.empty()) {
                throw std::runtime_error(
                    "No universe candles were returned for session " + date);
            }

            for (OptimizationVariant& variant : variants) {
                zerotick::BacktestConfig dailyConfig = variant.config;
                dailyConfig.initialCapital = variant.capital;
                const zerotick::BacktestResult result = engine.run(
                    universe, dayCandles, tickSizes, dailyConfig);
                variant.capital = result.endingCapital;
                addTradeStats(
                    nextSession < trainingDays
                        ? variant.training
                        : variant.holdout,
                    result.trades);
                if (nextSession + 1 == trainingDays) {
                    variant.trainingEndingCapital = variant.capital;
                    variant.holdoutPeakCapital = variant.capital;
                } else if (nextSession >= trainingDays) {
                    variant.holdoutPeakCapital = std::max(
                        variant.holdoutPeakCapital, variant.capital);
                    if (variant.holdoutPeakCapital > 0.0) {
                        variant.maximumHoldoutCloseDrawdownPercent =
                            std::max(
                                variant.maximumHoldoutCloseDrawdownPercent,
                                (variant.holdoutPeakCapital - variant.capital) /
                                    variant.holdoutPeakCapital * 100.0);
                    }
                }
            }

            ++nextSession;
            zerotick::logLine(
                "[optimization] sessions_processed=" +
                std::to_string(nextSession) + "/" +
                std::to_string(sessionCount) + " date=" + date);
        }
    }

    printOptimizationResults(variants, trainingDays, holdoutDays);
    zerotick::logLine(
        "[optimization] symbols_without_any_data=" +
        std::to_string(symbols.size() - symbolsWithData.size()));
}

} // namespace

int main(int argumentCount, char* arguments[]) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    try {
        bool forceLogin = false;
        bool liveTrading = false;
        bool backtest = false;
        bool optimize100Days = false;
        bool capitalProvided = false;
        std::string backtestDate;
        zerotick::BacktestConfig backtestConfig;
        for (int index = 1; index < argumentCount; ++index) {
            std::string argument(arguments[index]);
            if (argument == "--login") {
                forceLogin = true;
            } else if (argument == "--enable-live-trading") {
                liveTrading = true;
            } else if (argument == "--backtest") {
                if (backtest || index + 1 >= argumentCount) {
                    throw std::invalid_argument(
                        "--backtest requires one YYYY-MM-DD date");
                }
                backtest = true;
                backtestDate = arguments[++index];
            } else if (argument == "--optimize-100-days") {
                if (optimize100Days) {
                    throw std::invalid_argument(
                        "--optimize-100-days may only be specified once");
                }
                optimize100Days = true;
            } else if (argument == "--capital") {
                if (index + 1 >= argumentCount) {
                    throw std::invalid_argument(
                        "--capital requires a positive amount");
                }
                const std::string amount = arguments[++index];
                std::size_t parsedLength = 0;
                backtestConfig.initialCapital =
                    std::stod(amount, &parsedLength);
                if (parsedLength != amount.size() ||
                    !std::isfinite(backtestConfig.initialCapital) ||
                    backtestConfig.initialCapital <= 0.0) {
                    throw std::invalid_argument(
                        "--capital requires a positive numeric amount");
                }
                capitalProvided = true;
            } else if (argument == "--output") {
                throw std::invalid_argument(
                    "--output is no longer supported; backtest trades are "
                    "printed in a console table");
            } else {
                throw std::invalid_argument("Unknown argument: " + argument);
            }
        }
        if (backtest && optimize100Days) {
            throw std::invalid_argument(
                "--backtest and --optimize-100-days cannot be combined");
        }
        if ((backtest || optimize100Days) && liveTrading) {
            throw std::invalid_argument(
                "Historical analysis cannot be combined with "
                "--enable-live-trading");
        }
        if (!backtest && !optimize100Days && capitalProvided) {
            throw std::invalid_argument(
                "--capital can only be used with historical analysis");
        }

        zerotick::KiteConnectApi kiteApi(forceLogin);

        zerotick::NiftyUniverse universe;
        if (optimize100Days) {
            runOptimizationStudy(kiteApi, universe, backtestConfig);
            return 0;
        }
        if (backtest) {
            const std::vector<std::string> symbols = universe.symbols();
            std::map<std::string, double> tickSizes;
            std::size_t symbolsWithoutData = 0;
            const auto candles = downloadBacktestCandles(
                kiteApi, symbols, backtestDate, tickSizes,
                symbolsWithoutData);
            if (candles.empty()) {
                throw std::runtime_error(
                    "Kite returned no historical candles for " + backtestDate);
            }

            zerotick::BacktestEngine engine;
            const zerotick::BacktestResult result = engine.run(
                universe, candles, tickSizes, backtestConfig);
            printBacktestSummary(
                backtestDate, symbolsWithoutData, result);
            return 0;
        }

        std::vector<std::string> symbols = universe.symbols();
        zerotick::ThreadSafeQueue<zerotick::MarketSnapshot> quoteUpdates;
        refreshQuotesInChunks(kiteApi, symbols, 40, quoteUpdates);

        double totalAvailableCash = 0.0;
        if (liveTrading) {
            kiteApi.ensureNoOpenMisExposure(symbols);
            totalAvailableCash = kiteApi.cashAvailability().freeCash;
            zerotick::logLine(
                "[kite] live_trading=enabled symbols=" +
                std::to_string(symbols.size()) +
                " cash_budget=" + std::to_string(totalAvailableCash) +
                " max_positions=" +
                std::to_string(zerotick::kMaximumLivePositions) +
                " product=MIS");
        }

        zerotick::PortfolioRiskManager portfolioRisk(
            totalAvailableCash, zerotick::kMaximumLivePositions);

        zerotick::ThreadSafeQueue<zerotick::StockCandidate> selectionToDecision;
        zerotick::ThreadSafeQueue<zerotick::TradeSignal> decisionToHandling;
        zerotick::StockEngineRegistry registry;

        zerotick::SelectionConfig selectionConfig;
        zerotick::DecisionConfig decisionConfig;
        zerotick::HandlingConfig handlingConfig;
        handlingConfig.liveTrading = liveTrading;

        zerotick::StockSelectionEngine selectionEngine(
            quoteUpdates, registry, selectionToDecision, universe,
            selectionConfig);
        zerotick::TradingDecisionEngine decisionEngine(
            kiteApi, registry, selectionToDecision, decisionToHandling,
            decisionConfig);
        zerotick::TradeHandlingEngine handlingEngine(
            kiteApi, registry, decisionToHandling, portfolioRisk,
            handlingConfig);

        selectionEngine.start();
        decisionEngine.start();
        handlingEngine.start();

        std::thread quotePoller([&] {
            while (!shutdownRequested.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (shutdownRequested.load()) {
                    break;
                }
                try {
                    // Only returned quote snapshots enter selection. The API
                    // cache remains available for the decision-stage recheck.
                    refreshQuotesInChunks(kiteApi, symbols, 40, quoteUpdates);
                } catch (const std::exception& exception) {
                    zerotick::logLine(
                        std::string("[kite] quote_batch=failed reason=\"") +
                        exception.what() + std::string("\""));
                }
            }
        });

        while (!shutdownRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Shutdown requested; stopping engines\n";
        quotePoller.join();
        selectionEngine.stop();
        decisionEngine.stop();
        handlingEngine.stop();

        zerotick::logLine(
            "[engines] in_flight=" +
            std::to_string(
                registry.countInEngine(zerotick::EngineId::Selection) +
                registry.countInEngine(zerotick::EngineId::Decision) +
                registry.countInEngine(zerotick::EngineId::Handling)));
    } catch (const std::exception& exception) {
        std::cerr << "Fatal error: " << exception.what() << "\n";
        return 1;
    }

    return 0;
}
