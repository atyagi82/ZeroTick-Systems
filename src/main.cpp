#include "zerotick/KiteApi.h"
#include "zerotick/Log.h"
#include "zerotick/EngineTypes.h"
#include "zerotick/StockSelectionEngine.h"
#include "zerotick/TradingDecisionEngine.h"
#include "zerotick/TradeHandlingEngine.h"

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

} // namespace

int main(int argumentCount, char* arguments[]) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    try {
        bool forceLogin = false;
        bool liveTrading = false;
        for (int index = 1; index < argumentCount; ++index) {
            std::string argument(arguments[index]);
            if (argument == "--login") {
                forceLogin = true;
            } else if (argument == "--enable-live-trading") {
                liveTrading = true;
            } else {
                throw std::invalid_argument("Unknown argument: " + argument);
            }
        }

        zerotick::KiteConnectApi kiteApi(forceLogin);

        zerotick::NiftyUniverse universe;
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
