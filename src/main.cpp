#include "zerotick/KiteApi.h"
#include "zerotick/Log.h"
#include "zerotick/ThreadPool.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic_bool shutdownRequested{false};

void handleSignal(int) {
    shutdownRequested.store(true);
}

} // namespace

int main(int argumentCount, char* arguments[]) {
    // Keep the existing 20-symbol watchlist; each symbol gets one worker.
    const std::vector<std::string> symbols = {
        "YESBANK", "HDFCBANK", "ICICIBANK", "INFY", "BHARTIARTL",
        "TCS", "SBIN", "ITC", "LT", "AXISBANK",
        "HINDUNILVR", "KOTAKBANK", "BAJFINANCE", "M&M", "MARUTI",
        "SUNPHARMA", "TITAN", "ONGC", "NTPC", "ULTRACEMCO"
    };

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    try {
        bool forceLogin = false;
        bool liveTrading = false;
        for (int index = 1; index < argumentCount; ++index) {
            if (std::string(arguments[index]) == "--login") {
                forceLogin = true;
            } else if (std::string(arguments[index]) ==
                       "--enable-live-trading") {
                liveTrading = true;
            } else {
                throw std::invalid_argument(
                    "Unknown argument: " + std::string(arguments[index]));
            }
        }

        // Authentication and the first batch quote request fail fast if the
        // local credentials are missing, expired, or rejected by Kite.
        zerotick::KiteConnectApi kiteApi(forceLogin);
        kiteApi.refreshQuotes(symbols);
        double totalAvailableCash = 0.0;
        if (liveTrading) {
            kiteApi.ensureNoOpenMisExposure(symbols);
            totalAvailableCash = kiteApi.cashAvailability().freeCash;
            zerotick::logLine(
                "[kite] live_trading=enabled symbols=" +
                std::to_string(symbols.size()) +
                " portfolio_cash_budget=" +
                std::to_string(totalAvailableCash) +
                " equal_slot_budget=" +
                std::to_string(
                    totalAvailableCash / zerotick::kMaximumLivePositions) +
                " max_open_positions=" +
                std::to_string(zerotick::kMaximumLivePositions) +
                " product=MIS");
        }
        zerotick::logLine(
            "[kite] quote_batch=received instruments=" +
            std::to_string(symbols.size()));
        zerotick::ThreadPool pool(
            kiteApi,
            symbols,
            std::chrono::seconds(1),
            liveTrading,
            totalAvailableCash);
        pool.start();

        // Fetch one batched quote response per second, not 20 concurrent
        // REST requests from individual stock workers.
        std::thread quotePoller([&] {
            while (!shutdownRequested.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (shutdownRequested.load()) {
                    break;
                }
                try {
                    kiteApi.refreshQuotes(symbols);
                } catch (const std::exception& exception) {
                    zerotick::logLine(
                        std::string("[kite] quote_batch=failed reason=\"") +
                        exception.what() + '"');
                }
            }
        });

        while (!shutdownRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Shutdown requested; stopping workers\n";
        quotePoller.join();
        pool.stop();
    } catch (const std::exception& exception) {
        std::cerr << "Fatal error: " << exception.what() << '\n';
        return 1;
    }

    return 0;
}
