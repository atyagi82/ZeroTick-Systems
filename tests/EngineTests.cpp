#include "zerotick/EngineTypes.h"
#include "zerotick/BacktestEngine.h"
#include "zerotick/StockSelectionEngine.h"
#include "zerotick/TradingDecisionEngine.h"
#include "zerotick/TradeHandlingEngine.h"
#include "zerotick/Risk.h"
#include "zerotick/RuntimeConfig.h"
#include "zerotick/Log.h"
#include "zerotick/StructuredEventWriter.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

zerotick::MarketSnapshot makeQuote(double lastPrice, std::uint64_t volume,
                                   double bidQty, double askQty,
                                   double openPrice, double highPrice,
                                   double lowPrice) {
    zerotick::MarketSnapshot snapshot;
    snapshot.symbol = "TEST";
    snapshot.lastPrice = lastPrice;
    snapshot.bidPrice = lastPrice - 0.01;
    snapshot.askPrice = lastPrice + 0.01;
    snapshot.volume = volume;
    snapshot.openPrice = openPrice;
    snapshot.highPrice = highPrice;
    snapshot.lowPrice = lowPrice;
    snapshot.receivedAt = std::chrono::steady_clock::now();
    snapshot.bidLevels[0].price = snapshot.bidPrice;
    snapshot.bidLevels[0].quantity = static_cast<std::uint64_t>(bidQty);
    snapshot.bidQuantity = static_cast<std::uint64_t>(bidQty);
    snapshot.askLevels[0].price = snapshot.askPrice;
    snapshot.askLevels[0].quantity = static_cast<std::uint64_t>(askQty);
    snapshot.askQuantity = static_cast<std::uint64_t>(askQty);
    return snapshot;
}

class TestUniverse : public zerotick::StockUniverse {
public:
    explicit TestUniverse(std::vector<std::string> symbols)
        : symbols_(std::move(symbols)) {}
    std::vector<std::string> symbols() const override { return symbols_; }
    bool contains(const std::string& symbol) const override {
        for (const std::string& s : symbols_) {
            if (s == symbol) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::string> symbols_;
};

zerotick::HistoricalCandle makeCandle(
    const std::string& timestamp,
    double open,
    double high,
    double low,
    double close,
    std::uint64_t volume) {
    zerotick::HistoricalCandle candle;
    candle.timestamp = timestamp;
    candle.open = open;
    candle.high = high;
    candle.low = low;
    candle.close = close;
    candle.volume = volume;
    return candle;
}

class FakeKiteApi : public zerotick::KiteApi {
public:
    explicit FakeKiteApi(const zerotick::MarketSnapshot& quote) : quote_(quote) {}

    zerotick::MarketSnapshot getQuote(
        const std::string&,
        std::chrono::seconds = std::chrono::seconds(5)) override {
        return quote_;
    }
    std::vector<zerotick::MarketSnapshot> refreshQuotes(
        const std::vector<std::string>&) override {
        return {};
    }
    void ensureNoOpenMisExposure(const std::vector<std::string>&) override {}
    zerotick::CashAvailability cashAvailability() override {
        return {100000.0, 0.0, 0.0, 100000.0, 0.0, 0.0, 100000.0};
    }
    zerotick::OrderResult placeStopLimitEntry(
        const std::string&, int, double, double,
        zerotick::PositionSide) override {
        return {true, "order", "submitted"};
    }
    zerotick::OrderResult placeLimitExit(
        const std::string&, int, double, zerotick::PositionSide) override {
        return {true, "order", "submitted"};
    }
    zerotick::BrokerOrderStatus getOrderStatus(const std::string&) override {
        return {"COMPLETE", 0, 0.0, true};
    }
    void cancelOrder(const std::string&) override {}
    bool isHealthy() const override { return true; }

private:
    zerotick::MarketSnapshot quote_;
};

void testNiftyUniverseUsesCurrentNifty100Symbols() {
    zerotick::NiftyUniverse universe;
    const std::vector<std::string> symbols = universe.symbols();
    require(symbols.size() == 100, "NIFTY 100 should contain 100 symbols");
    require(universe.contains("ETERNAL"), "current Eternal symbol included");
    require(universe.contains("LTM"), "current LTM symbol included");
    require(universe.contains("TMCV"), "current Tata Motors CV symbol included");
    require(universe.contains("TMPV"), "current Tata Motors PV symbol included");
    require(!universe.contains("AARTI"), "invalid AARTI symbol excluded");
    require(!universe.contains("AUROBINDO"),
            "non-NIFTY-100 AUROBINDO symbol excluded");
    require(!universe.contains("CARDEC"), "invalid CARDEC symbol excluded");
    require(!universe.contains("DEEPAKNITRE"),
            "invalid DEEPAKNITRE symbol excluded");
    require(!universe.contains("GE"), "invalid GE symbol excluded");
    require(!universe.contains("ZOMATO"),
            "outdated ZOMATO symbol excluded");
}

void testLogLinePersistsToLogsFile() {
    zerotick::initializeLogFile();
    const std::string marker = "[test] persistent_log_marker";
    zerotick::logLine(marker);

    std::ifstream input("logs.txt");
    require(static_cast<bool>(input), "logs.txt should be created");
    bool markerFound = false;
    std::string line;
    while (std::getline(input, line)) {
        if (line.find(marker) != std::string::npos) {
            markerFound = true;
            break;
        }
    }
    require(markerFound, "logLine should append timestamped file records");
}

void testRegistryOneEngineAtATime() {
    zerotick::StockEngineRegistry registry;

    require(registry.claim("TEST", zerotick::EngineId::Unassigned,
                           zerotick::EngineId::Selection),
            "claim unassigned -> selection");
    // Same symbol cannot be claimed by another engine while Selection owns it.
    require(!registry.claim("TEST", zerotick::EngineId::Unassigned,
                            zerotick::EngineId::Decision),
            "decision cannot steal a selection-owned symbol");
    require(!registry.claim("TEST", zerotick::EngineId::Selection,
                            zerotick::EngineId::Selection),
            "re-claiming the same owner is not allowed");
    require(registry.ownerOf("TEST") == zerotick::EngineId::Selection,
            "owner is selection");

    // Hand off to the decision engine.
    require(registry.claim("TEST", zerotick::EngineId::Selection,
                           zerotick::EngineId::Decision),
            "claim selection -> decision");
    require(registry.ownerOf("TEST") == zerotick::EngineId::Decision,
            "owner is decision");

    // Hand off to the handling engine.
    require(registry.claim("TEST", zerotick::EngineId::Decision,
                           zerotick::EngineId::Handling),
            "claim decision -> handling");
    require(registry.ownerOf("TEST") == zerotick::EngineId::Handling,
            "owner is handling");

    // Release back to unassigned.
    require(registry.release("TEST", zerotick::EngineId::Handling),
            "release handling");
    require(registry.ownerOf("TEST") == zerotick::EngineId::Unassigned,
            "back to unassigned");
    require(registry.isUnassigned("TEST"), "is unassigned");

    // Releasing an unowned symbol fails.
    require(!registry.release("TEST", zerotick::EngineId::Selection),
            "release unowned symbol fails");
}

void testSelectionSelectsGoodStock() {
    TestUniverse universe({"TEST"});
    zerotick::StockEngineRegistry registry;
    zerotick::ThreadSafeQueue<zerotick::MarketSnapshot> quoteUpdates;
    zerotick::MarketSnapshot baseline =
        makeQuote(150.0, 200000, 5000, 1000, 149.0, 151.0, 148.0);
    zerotick::MarketSnapshot rising =
        makeQuote(150.15, 200000, 5000, 1000, 149.0, 151.0, 148.0);
    rising.receivedAt = baseline.receivedAt + std::chrono::milliseconds(1);
    quoteUpdates.push(baseline);
    quoteUpdates.push(rising);
    zerotick::ThreadSafeQueue<zerotick::StockCandidate> output;
    zerotick::SelectionConfig config;
    config.workerCount = 1;
    config.minPrice = 100.0;
    config.minAbsoluteVolume = 1000;
    config.minRelativeVolume = 0.0;
    config.maxSpreadPercent = 0.5;
    config.minDepthQuantity = 100;
    config.minIntradayRangePercent = 0.0;
    config.maxPriceChangePercent = 100.0;

    zerotick::StockSelectionEngine engine(quoteUpdates, registry, output,
                                          universe, config);
    engine.start();

    zerotick::StockCandidate candidate;
    require(output.pop(candidate, std::chrono::seconds(2)),
            "selection should emit a candidate for a good stock");
    require(candidate.symbol == "TEST", "candidate symbol");
    require(candidate.lastPrice == 150.15, "candidate uses latest quote price");
    require(
        std::abs(candidate.priceChangePercent - 0.1) < 0.001,
        "candidate price change should compare against the previous quote");
    require(candidate.inNifty100, "candidate in nifty 100");
    require(candidate.relativeVolume > 0.0, "relative volume computed");

    require(registry.release("TEST", zerotick::EngineId::Selection),
            "release first candidate to process later quote events");
    zerotick::MarketSnapshot delayed =
        makeQuote(150.0, 200000, 5000, 1000, 149.0, 151.0, 148.0);
    zerotick::MarketSnapshot latest =
        makeQuote(150.23, 200000, 5000, 1000, 149.0, 151.0, 148.0);
    delayed.receivedAt = baseline.receivedAt + std::chrono::milliseconds(1);
    latest.receivedAt = baseline.receivedAt + std::chrono::milliseconds(3);
    quoteUpdates.push(delayed);
    quoteUpdates.push(latest);

    require(output.pop(candidate, std::chrono::seconds(2)),
            "selection should ignore an out-of-order quote and process latest");
    require(
        std::abs(candidate.priceChangePercent - (0.08 / 150.15 * 100.0)) <
            0.001,
        "an older quote must not roll the price baseline backward");

    engine.stop();
}

void testSelectionSelectsShortOnDownwardMomentumWhenEnabled() {
    TestUniverse universe({"TEST"});
    zerotick::StockEngineRegistry registry;
    zerotick::ThreadSafeQueue<zerotick::MarketSnapshot> quoteUpdates;
    zerotick::ThreadSafeQueue<zerotick::StockCandidate> output;
    zerotick::SelectionConfig config;
    config.workerCount = 1;
    config.minPrice = 100.0;
    config.minAbsoluteVolume = 1000;
    config.minRelativeVolume = 0.0;
    config.minPriceChangePercent = 0.05;
    config.maxSpreadPercent = 1.0;
    config.minDepthQuantity = 100;
    config.minIntradayRangePercent = 0.0;
    config.allowShortSelling = true;

    zerotick::StockSelectionEngine engine(
        quoteUpdates, registry, output, universe, config);
    engine.start();

    zerotick::MarketSnapshot baseline =
        makeQuote(150.0, 200000, 5000, 1000, 149.0, 151.0, 148.0);
    zerotick::MarketSnapshot falling =
        makeQuote(149.85, 201000, 5000, 1000, 149.0, 151.0, 148.0);
    falling.receivedAt =
        baseline.receivedAt + std::chrono::milliseconds(1);
    quoteUpdates.push(baseline);
    quoteUpdates.push(falling);

    zerotick::StockCandidate candidate;
    require(output.pop(candidate, std::chrono::seconds(2)),
            "selection should accept qualifying downward momentum");
    require(candidate.side == zerotick::PositionSide::Short,
            "negative momentum should select a short side");
    engine.stop();
}

void testSelectionKeepsShortSellingDisabledByDefault() {
    TestUniverse universe({"TEST"});
    zerotick::StockEngineRegistry registry;
    zerotick::ThreadSafeQueue<zerotick::MarketSnapshot> quoteUpdates;
    zerotick::ThreadSafeQueue<zerotick::StockCandidate> output;
    zerotick::SelectionConfig config;
    config.workerCount = 1;
    config.minPrice = 100.0;
    config.minAbsoluteVolume = 1000;
    config.minRelativeVolume = 0.0;
    config.minPriceChangePercent = 0.05;
    config.maxSpreadPercent = 1.0;
    config.minDepthQuantity = 100;
    config.minIntradayRangePercent = 0.0;

    zerotick::StockSelectionEngine engine(
        quoteUpdates, registry, output, universe, config);
    engine.start();

    zerotick::MarketSnapshot baseline =
        makeQuote(150.0, 200000, 5000, 1000, 149.0, 151.0, 148.0);
    zerotick::MarketSnapshot falling =
        makeQuote(149.85, 201000, 5000, 1000, 149.0, 151.0, 148.0);
    falling.receivedAt =
        baseline.receivedAt + std::chrono::milliseconds(1);
    quoteUpdates.push(baseline);
    quoteUpdates.push(falling);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.stop();

    require(output.empty(),
            "downward momentum must not create shorts without opt-in");
}

void testSelectionRejectsPriceChangeBelowMinimum() {
    TestUniverse universe({"TEST"});
    zerotick::StockEngineRegistry registry;
    zerotick::ThreadSafeQueue<zerotick::MarketSnapshot> quoteUpdates;
    zerotick::MarketSnapshot baseline =
        makeQuote(150.0, 200000, 5000, 1000, 149.0, 151.0, 148.0);
    zerotick::MarketSnapshot smallRise =
        makeQuote(150.06, 200000, 5000, 1000, 149.0, 151.0, 148.0);
    smallRise.receivedAt = baseline.receivedAt + std::chrono::milliseconds(1);
    quoteUpdates.push(baseline);
    quoteUpdates.push(smallRise);
    zerotick::ThreadSafeQueue<zerotick::StockCandidate> output;
    zerotick::SelectionConfig config;
    config.workerCount = 1;
    config.minAbsoluteVolume = 1000;
    config.minRelativeVolume = 0.0;
    config.maxSpreadPercent = 0.5;
    config.minDepthQuantity = 100;
    config.minIntradayRangePercent = 0.0;
    config.maxPriceChangePercent = 100.0;

    zerotick::StockSelectionEngine engine(quoteUpdates, registry, output,
                                          universe, config);
    engine.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require(output.empty(),
            "price increase below minPriceChangePercent must be rejected");
    require(registry.isUnassigned("TEST"),
            "rejected low-momentum symbol should be released");
    engine.stop();
}

void testSelectionRejectsLowPrice() {
    zerotick::MarketSnapshot quote = makeQuote(50.0, 200000, 5000, 1000,
                                               49.0, 51.0, 49.0);
    TestUniverse universe({"TEST"});
    zerotick::StockEngineRegistry registry;
    zerotick::ThreadSafeQueue<zerotick::MarketSnapshot> quoteUpdates;
    quoteUpdates.push(quote);
    zerotick::ThreadSafeQueue<zerotick::StockCandidate> output;
    zerotick::SelectionConfig config;
    config.workerCount = 1;
    config.minPrice = 100.0;
    config.minAbsoluteVolume = 1000;
    config.minRelativeVolume = 0.0;
    config.maxSpreadPercent = 0.5;
    config.minDepthQuantity = 100;
    config.minIntradayRangePercent = 0.0;
    config.maxPriceChangePercent = 100.0;

    zerotick::StockSelectionEngine engine(quoteUpdates, registry, output,
                                          universe, config);
    engine.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require(output.empty(), "low-price stock must not be selected");
    engine.stop();
    require(registry.isUnassigned("TEST"), "rejected symbol back to unassigned");
}

void testSelectionRejectsLowVolume() {
    zerotick::MarketSnapshot quote = makeQuote(150.0, 100, 5000, 1000,
                                               149.0, 151.0, 148.0);
    TestUniverse universe({"TEST"});
    zerotick::StockEngineRegistry registry;
    zerotick::ThreadSafeQueue<zerotick::MarketSnapshot> quoteUpdates;
    quoteUpdates.push(quote);
    zerotick::ThreadSafeQueue<zerotick::StockCandidate> output;
    zerotick::SelectionConfig config;
    config.workerCount = 1;
    config.minPrice = 100.0;
    config.minAbsoluteVolume = 1000;
    config.minRelativeVolume = 0.0;
    config.maxSpreadPercent = 0.5;
    config.minDepthQuantity = 100;
    config.minIntradayRangePercent = 0.0;
    config.maxPriceChangePercent = 100.0;

    zerotick::StockSelectionEngine engine(quoteUpdates, registry, output,
                                          universe, config);
    engine.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require(output.empty(), "low-volume stock must not be selected");
    engine.stop();
    require(registry.isUnassigned("TEST"), "rejected symbol back to unassigned");
}

void testSelectionWaitsUntilQuoteAvailable() {
    TestUniverse universe({"TEST"});
    zerotick::StockEngineRegistry registry;
    zerotick::ThreadSafeQueue<zerotick::MarketSnapshot> quoteUpdates;
    zerotick::ThreadSafeQueue<zerotick::StockCandidate> output;
    zerotick::SelectionConfig config;
    config.workerCount = 1;
    config.minAbsoluteVolume = 1000;
    config.minRelativeVolume = 0.0;
    config.maxSpreadPercent = 0.5;
    config.minDepthQuantity = 100;
    config.minIntradayRangePercent = 0.0;
    config.maxPriceChangePercent = 100.0;

    zerotick::StockSelectionEngine engine(quoteUpdates, registry, output,
                                          universe, config);
    engine.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(output.empty(), "selection should wait while no quote is available");

    zerotick::MarketSnapshot baseline =
        makeQuote(150.0, 200000, 5000, 1000, 149.0, 151.0, 148.0);
    zerotick::MarketSnapshot rising =
        makeQuote(150.15, 200000, 5000, 1000, 149.0, 151.0, 148.0);
    rising.receivedAt = baseline.receivedAt + std::chrono::milliseconds(1);
    quoteUpdates.push(baseline);
    quoteUpdates.push(rising);
    zerotick::StockCandidate candidate;
    require(output.pop(candidate, std::chrono::seconds(2)),
            "selection should process rising quote after baseline arrives");
    engine.stop();
}

void testDecisionEmitsSignal() {
    zerotick::MarketSnapshot quote = makeQuote(150.0, 200000, 5000, 1000,
                                               149.0, 151.0, 148.0);
    FakeKiteApi api(quote);
    zerotick::StockEngineRegistry registry;
    require(registry.claim("TEST", zerotick::EngineId::Unassigned,
                           zerotick::EngineId::Selection),
            "pre-claim selection");

    zerotick::ThreadSafeQueue<zerotick::StockCandidate> input;
    zerotick::ThreadSafeQueue<zerotick::TradeSignal> output;
    zerotick::StockCandidate candidate;
    candidate.symbol = "TEST";
    candidate.lastPrice = 150.0;
    candidate.volume = 200000;
    candidate.relativeVolume = 2.0;
    candidate.spreadPercent = 0.01;
    input.push(candidate);

    zerotick::DecisionConfig config;
    config.workerCount = 1;
    config.pollInterval = std::chrono::milliseconds(10);
    config.minimumImbalance = 0.0;
    config.maximumSpreadPercent = 1.0;
    config.requireVolumeConfirmation = false;

    zerotick::TradingDecisionEngine engine(api, registry, input, output,
                                           config);
    engine.start();

    zerotick::TradeSignal signal;
    require(output.pop(signal, std::chrono::seconds(2)),
            "decision should emit a trade signal");
    require(signal.symbol == "TEST", "signal symbol");
    require(signal.entryTriggerPrice > 0.0, "entry trigger set");
    require(signal.entryLimitPrice > 0.0, "entry limit set");
    require(signal.targetPrice > signal.entryLimitPrice,
            "target above entry limit");
    require(signal.stopLossPrice < signal.entryLimitPrice,
            "stop below entry limit");
    require(std::abs(
                signal.targetPrice -
                signal.entryLimitPrice * 1.003) < 1e-8,
            "default target is 0.3 percent above entry limit");
    require(std::abs(
                signal.stopLossPrice -
                signal.entryLimitPrice * 0.996) < 1e-8,
            "default stop is 0.4 percent below entry limit");
    require(signal.quantity > 0, "signal quantity set");

    engine.stop();
}

void testDecisionEmitsShortSignalWithDownwardOrderBookPressure() {
    zerotick::MarketSnapshot quote =
        makeQuote(150.0, 200000, 1000, 5000, 149.0, 151.0, 148.0);
    FakeKiteApi api(quote);
    zerotick::StockEngineRegistry registry;
    require(registry.claim("TEST", zerotick::EngineId::Unassigned,
                           zerotick::EngineId::Selection),
            "pre-claim selection for short candidate");

    zerotick::ThreadSafeQueue<zerotick::StockCandidate> input;
    zerotick::ThreadSafeQueue<zerotick::TradeSignal> output;
    zerotick::StockCandidate candidate;
    candidate.symbol = "TEST";
    candidate.side = zerotick::PositionSide::Short;
    candidate.lastPrice = 150.0;
    candidate.relativeVolume = 2.0;
    input.push(candidate);

    zerotick::DecisionConfig config;
    config.allowShortSelling = true;
    config.workerCount = 1;
    config.pollInterval = std::chrono::milliseconds(10);
    config.minimumImbalance = 0.0;
    config.maximumSpreadPercent = 1.0;
    config.requireVolumeConfirmation = false;

    zerotick::TradingDecisionEngine engine(api, registry, input, output,
                                           config);
    engine.start();

    zerotick::TradeSignal signal;
    require(output.pop(signal, std::chrono::seconds(2)),
            "decision should emit an enabled short signal");
    require(signal.side == zerotick::PositionSide::Short,
            "signal should retain short direction");
    require(signal.entryTriggerPrice > signal.entryLimitPrice,
            "sell stop-limit should place its limit below the trigger");
    require(signal.targetPrice < signal.entryLimitPrice,
            "short target should be below entry");
    require(signal.stopLossPrice > signal.entryLimitPrice,
            "short stop should be above entry");
    require(std::abs(
                signal.targetPrice -
                signal.entryLimitPrice * 0.997) < 1e-8,
            "short target should use the configured target percentage");
    require(std::abs(
                signal.stopLossPrice -
                signal.entryLimitPrice * 1.004) < 1e-8,
            "short stop should use the configured stop percentage");
    engine.stop();
}

void testHandlingPaperModeRoundTrip() {
    zerotick::StockEngineRegistry registry;
    require(registry.claim("TEST", zerotick::EngineId::Unassigned,
                           zerotick::EngineId::Decision),
            "pre-claim decision");

    zerotick::ThreadSafeQueue<zerotick::TradeSignal> input;
    zerotick::PortfolioRiskManager portfolioRisk(100000.0);
    FakeKiteApi api(makeQuote(150.0, 200000, 5000, 1000, 149.0, 151.0, 148.0));
    zerotick::HandlingConfig config;
    config.workerCount = 1;
    config.pollInterval = std::chrono::milliseconds(10);
    config.liveTrading = false;

    zerotick::TradeHandlingEngine engine(api, registry, input, portfolioRisk,
                                         config);
    engine.start();

    zerotick::TradeSignal signal;
    signal.symbol = "TEST";
    signal.quantity = 10;
    signal.entryTriggerPrice = 150.02;
    signal.entryLimitPrice = 150.03;
    signal.targetPrice = 150.33;
    signal.stopLossPrice = 149.43;
    input.push(signal);

    bool released = false;
    for (int i = 0; i < 200 && !released; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (registry.isUnassigned("TEST")) {
            released = true;
        }
    }
    require(released, "handling should release symbol after paper round trip");

    engine.stop();
}

void testQuantityUsesFullEqualSlotBudget() {
    require(
        zerotick::calculateQuantityWithinBudget(1000.0, 50.10) == 19,
        "quantity should use the MIS margin requirement within one slot");
    require(
        zerotick::calculateQuantityWithinBudget(10.0, 17.90) == 0,
        "an equal slot below one-share cost must not produce a quantity");
}

void testPortfolioRiskManagerLimitsAndDividesSlotsEqually() {
    zerotick::PortfolioRiskManager risk(1000.0);
    for (int index = 0; index < 10; ++index) {
        double slotBudget = 0.0;
        require(
            risk.reserveSlot("SYMBOL" + std::to_string(index), 1000.0,
                             slotBudget),
            "each of ten symbols should reserve one portfolio slot");
        require(std::abs(slotBudget - 100.0) < 0.0001,
                "portfolio cash should be divided across ten slots");
    }

    double extraSlotBudget = 0.0;
    require(!risk.reserveSlot("EXTRA", 1000.0, extraSlotBudget),
            "a position beyond the ten-slot limit must be blocked");
    risk.releaseSlot("SYMBOL0");
    require(risk.reserveSlot("EXTRA", 1000.0, extraSlotBudget),
            "closing a position should release its portfolio slot");
}

void testFreeCashSubtractsExistingUtilization() {
    require(
        std::abs(zerotick::calculateFreeCash(
                     245431.60, 0.0, 145706.55, 99725.05) -
                 99725.05) < 0.0001,
        "free cash should exclude already utilized funds");
    require(
        zerotick::calculateFreeCash(500.0, 0.0, 600.0, 900.0) == 0.0,
        "utilization greater than cash should leave no spendable budget");
    require(
        zerotick::calculateFreeCash(1000.0, 0.0, 0.0, 500.0) == 500.0,
        "free cash should not exceed Kite's current live balance");
    require(
        zerotick::calculateFreeCash(0.30, 101.0, 0.0, 101.30) == 101.30,
        "intraday payin should be included but capped at live balance");
}

void testRuntimeConfigLoadsAlgorithmParameters() {
    const zerotick::RuntimeConfig config = zerotick::parseRuntimeConfig(
        R"({"run":{"data_output_max_mb":64},"allow_short_selling":true,"market_data":{"quote_poll_interval_ms":750,"quote_max_age_seconds":8,"fallback_tick_size":0.05},"selection":{"min_price":125.0,"volume_ewma_alpha":0.3,"worker_count":2},"decision":{"target_percent":0.3,"stop_percent":0.4,"indicative_entry_budget":25000.0,"imbalance_level_weights":[1.0,0.7,0.5,0.3,0.1],"entry_trigger_offset_ticks":2,"entry_limit_offset_ticks":3},"risk":{"max_open_positions":5,"paper_trading_cash":50000.0},"backtest":{"initial_capital":20000.0,"max_open_positions":3,"entry_timeout_minutes":7,"entry_start_time":"12:00","entry_end_time":"13:00"}})");
    require(config.run.dataOutputMaxMb == 64,
            "structured output rotation size should load from configuration");
    require(config.allowShortSelling,
            "configuration should allow explicitly enabled short selling");
    require(config.selection.minPrice == 125.0 &&
                config.selection.workerCount == 2 &&
                config.selection.volumeEwmaAlpha == 0.3,
            "selection parameters should load from configuration");
    require(config.decision.targetPercent == 0.3 &&
                config.decision.stopPercent == 0.4 &&
                config.decision.indicativeEntryBudget == 25000.0 &&
                config.decision.imbalanceLevelWeights[1] == 0.7 &&
                config.decision.entryTriggerOffsetTicks == 2 &&
                config.decision.entryLimitOffsetTicks == 3,
            "decision and exit parameters should load from configuration");
    require(config.maxOpenPositions == 5 &&
                config.backtest.maxOpenPositions == 3,
            "live and backtest position caps should be independently configured");
    require(config.backtest.initialCapital == 20000.0 &&
                config.backtest.entryStartTime == "12:00" &&
                config.backtest.entryEndTime == "13:00" &&
                config.backtest.entryTimeoutMinutes == 7 &&
                config.backtest.fallbackTickSize == 0.05,
            "backtest capital, window, tick, and entry timeout should load");
    require(config.quotePollInterval.count() == 750 &&
                config.selection.maxQuoteAge.count() == 8 &&
                config.handling.maxQuoteAge.count() == 8 &&
                config.paperTradingCash == 50000.0 &&
                config.handling.paperTradingCash == 50000.0,
            "market-data and paper-risk settings should reach runtime engines");
    require(config.decision.fallbackTickSize == 0.05 &&
                config.backtest.decision.entryTriggerOffsetTicks == 2,
            "shared tick and entry settings should reach the relevant engines");
    require(config.selection.allowShortSelling &&
                config.decision.allowShortSelling,
            "short-selling setting should reach the engine configs");
}

void testStructuredEventWriterRotatesCompleteJsonLines() {
    const std::string base = "structured_rotation_test.jsonl";
    const std::string second = "structured_rotation_test.part002.jsonl";
    const std::string third = "structured_rotation_test.part003.jsonl";
    std::remove(base.c_str());
    std::remove(second.c_str());
    std::remove(third.c_str());
    {
        zerotick::StructuredEventWriter writer(base, 4000);
        writer.recordSession("shadow_live");
        zerotick::MarketSnapshot snapshot = makeQuote(
            100.0, 100000, 5000, 5000, 99.0, 101.0, 98.0);
        for (int index = 0; index < 10; ++index) {
            snapshot.sequence = static_cast<std::uint64_t>(index);
            writer.recordSnapshot(snapshot);
        }
    }
    std::ifstream rotated(second, std::ios::in | std::ios::binary);
    require(static_cast<bool>(rotated),
            "structured writer should create a numbered second part");
    std::size_t records = 0;
    for (const std::string& path : {base, second, third}) {
        std::ifstream input(path, std::ios::in | std::ios::binary);
        if (!input) continue;
        input.seekg(0, std::ios::end);
        require(input.tellg() <= 4000,
                "rotated structured files should respect their size limit");
        input.seekg(0, std::ios::beg);
        std::string line;
        while (std::getline(input, line)) {
            require(!line.empty() && line.front() == '{' && line.back() == '}',
                    "rotation must preserve complete JSON records");
            ++records;
        }
    }
    require(records == 11,
            "rotation should retain the session and every snapshot record");
    std::remove(base.c_str());
    std::remove(second.c_str());
    std::remove(third.c_str());
}

void testRuntimeConfigRejectsUnknownAndInvalidValues() {
    bool unknownKeyRejected = false;
    try {
        zerotick::parseRuntimeConfig(R"({"decision":{"typo":1}})");
    } catch (const std::invalid_argument&) {
        unknownKeyRejected = true;
    }
    require(unknownKeyRejected,
            "unknown configuration keys should be reported");

    bool invalidWindowRejected = false;
    try {
        zerotick::parseRuntimeConfig(
            R"({"backtest":{"entry_start_time":"13:00","entry_end_time":"12:00"}})");
    } catch (const std::invalid_argument&) {
        invalidWindowRejected = true;
    }
    require(invalidWindowRejected,
            "invalid backtest entry windows should be rejected");

    bool invalidTargetRejected = false;
    try {
        zerotick::parseRuntimeConfig(
            R"({"decision":{"target_percent":0.0}})");
    } catch (const std::invalid_argument&) {
        invalidTargetRejected = true;
    }
    require(invalidTargetRejected,
            "nonpositive exit targets should be rejected");

    bool negativePositionLimitRejected = false;
    try {
        zerotick::parseRuntimeConfig(
            R"({"risk":{"max_open_positions":-1}})");
    } catch (const std::invalid_argument&) {
        negativePositionLimitRejected = true;
    }
    require(negativePositionLimitRejected,
            "negative position limits should be rejected");
}

void testCheckedInRuntimeConfigLoads() {
    const zerotick::RuntimeConfig config =
        zerotick::loadRuntimeConfig("config.json");
    require(config.decision.targetPercent > 0.0 &&
                config.decision.stopPercent > 0.0 &&
                config.backtest.initialCapital > 0.0,
            "checked-in algorithm settings should be valid and loaded");
    require(config.backtest.maxOpenPositions > 0,
            "checked-in backtest position cap should be loaded");
    require(config.backtest.entryTimeoutMinutes == 5,
            "checked-in backtest entry timeout should be five minutes");
}

void testBacktestPositionCapIsIndependentByDefault() {
    const zerotick::RuntimeConfig config = zerotick::parseRuntimeConfig(
        R"({"risk":{"max_open_positions":100},"backtest":{"initial_capital":10000.0}})");
    require(config.maxOpenPositions == 100,
            "live risk position cap should use risk.max_open_positions");
    require(config.backtest.maxOpenPositions == 10,
            "backtest should retain its independent default position cap");
}

void testBacktestReportsInsufficientPerPositionBudget() {
    TestUniverse universe({"TEST"});
    std::map<std::string, std::vector<zerotick::HistoricalCandle>> data;
    data["TEST"] = {
        makeCandle("2026-11-18T12:00:00+0530",
                   150.0, 151.0, 149.0, 150.0, 100000),
        makeCandle("2026-11-18T12:01:00+0530",
                   150.0, 151.2, 149.9, 150.15, 1000)
    };

    zerotick::BacktestConfig config;
    config.initialCapital = 10000.0;
    config.maxOpenPositions = 100;
    config.selection.minAbsoluteVolume = 1;
    config.selection.minRelativeVolume = 0.0;
    config.selection.minIntradayRangePercent = 0.3;
    config.decision.requireVolumeConfirmation = false;

    zerotick::BacktestEngine engine;
    const zerotick::BacktestResult result =
        engine.run(universe, data, {{"TEST", 0.01}}, config);
    require(result.generatedSignals == 1,
            "a qualified signal should be counted before risk sizing");
    require(result.riskRejectedSignals == 1 &&
                result.insufficientBudgetRejectedSignals == 1 &&
                result.positionLimitRejectedSignals == 0,
            "budget rejection should be distinguished from the position cap");
}

void testBacktestUsesNextCandleEntryAndTargetExit() {
    TestUniverse universe({"TEST"});
    std::map<std::string, std::vector<zerotick::HistoricalCandle>> data;
    data["TEST"] = {
        makeCandle("2026-11-18T12:00:00+0530",
                   150.0, 151.0, 149.0, 150.0, 100000),
        makeCandle("2026-11-18T12:01:00+0530",
                   150.0, 151.2, 149.9, 150.15, 1000),
        makeCandle("2026-11-18T12:02:00+0530",
                   150.15, 150.18, 150.14, 150.16, 1000),
        makeCandle("2026-11-18T12:03:00+0530",
                   150.2, 151.0, 150.0, 150.6, 1000)
    };
    std::map<std::string, double> tickSizes;
    tickSizes["TEST"] = 0.01;

    zerotick::BacktestConfig config;
    config.initialCapital = 10000.0;
    config.selection.minPrice = 100.0;
    config.selection.minAbsoluteVolume = 1;
    config.selection.minRelativeVolume = 0.0;
    config.selection.minIntradayRangePercent = 0.3;
    config.decision.requireVolumeConfirmation = false;

    zerotick::BacktestEngine engine;
    const zerotick::BacktestResult result =
        engine.run(universe, data, tickSizes, config);
    require(result.generatedSignals == 1, "one candle signal should qualify");
    require(result.filledEntries == 1, "next candle should fill the entry");
    require(result.trades.size() == 1, "one trade should close");
    require(result.trades[0].entryTime == "2026-11-18T12:02:00+0530",
            "entry must not fill on the signal candle");
    require(result.trades[0].exitReason == "target",
            "a later candle reaching target should exit the trade");
    require(result.trades[0].quantity == 6,
            "ten-thousand capital uses one-thousand position slots");
    require(result.grossProfitLoss > 0.0,
            "target exit should produce positive gross P&L");
    require(std::abs(result.endingCapital -
                     (result.initialCapital + result.grossProfitLoss)) <
                0.0001,
            "ending capital should reconcile to gross P&L");
}

void testBacktestKeepsEntryPendingForFiveMinutes() {
    TestUniverse universe({"TEST"});
    std::map<std::string, std::vector<zerotick::HistoricalCandle>> data;
    data["TEST"] = {
        makeCandle("2026-11-18T12:00:00+0530",
                   150.0, 151.0, 149.0, 150.0, 100000),
        makeCandle("2026-11-18T12:01:00+0530",
                   150.0, 151.2, 149.9, 150.15, 1000),
        makeCandle("2026-11-18T12:02:00+0530",
                   150.15, 150.15, 150.10, 150.14, 1000),
        makeCandle("2026-11-18T12:03:00+0530",
                   150.14, 150.15, 150.10, 150.13, 1000),
        makeCandle("2026-11-18T12:04:00+0530",
                   150.13, 150.15, 150.10, 150.12, 1000),
        makeCandle("2026-11-18T12:05:00+0530",
                   150.12, 150.15, 150.10, 150.11, 1000),
        makeCandle("2026-11-18T12:06:00+0530",
                   150.11, 150.25, 150.10, 150.20, 1000),
        makeCandle("2026-11-18T12:07:00+0530",
                   150.20, 151.0, 150.15, 150.8, 1000)
    };

    zerotick::BacktestConfig config;
    config.entryTimeoutMinutes = 5;
    config.selection.minAbsoluteVolume = 1;
    config.selection.minRelativeVolume = 0.0;
    config.selection.minIntradayRangePercent = 0.3;
    config.decision.requireVolumeConfirmation = false;

    zerotick::BacktestEngine engine;
    const zerotick::BacktestResult result =
        engine.run(universe, data, {{"TEST", 0.01}}, config);
    require(result.generatedSignals == 1,
            "the initial qualifying bar should create one signal");
    require(result.filledEntries == 1 && result.trades.size() == 1,
            "entry should fill when triggered within the five-minute window");
    require(result.trades[0].entryTime ==
                "2026-11-18T12:06:00+0530",
            "entry should remain eligible through the fifth subsequent minute");
}

void testBacktestExpiresEntryAfterFiveMinutes() {
    TestUniverse universe({"TEST"});
    std::map<std::string, std::vector<zerotick::HistoricalCandle>> data;
    data["TEST"] = {
        makeCandle("2026-11-18T12:00:00+0530",
                   150.0, 151.0, 149.0, 150.0, 100000),
        makeCandle("2026-11-18T12:01:00+0530",
                   150.0, 151.2, 149.9, 150.15, 1000),
        makeCandle("2026-11-18T12:02:00+0530",
                   150.15, 150.15, 150.10, 150.14, 1000),
        makeCandle("2026-11-18T12:03:00+0530",
                   150.14, 150.15, 150.10, 150.13, 1000),
        makeCandle("2026-11-18T12:04:00+0530",
                   150.13, 150.15, 150.10, 150.12, 1000),
        makeCandle("2026-11-18T12:05:00+0530",
                   150.12, 150.15, 150.10, 150.11, 1000),
        makeCandle("2026-11-18T12:06:00+0530",
                   150.11, 150.15, 150.10, 150.12, 1000)
    };

    zerotick::BacktestConfig config;
    config.entryTimeoutMinutes = 5;
    config.selection.minAbsoluteVolume = 1;
    config.selection.minRelativeVolume = 0.0;
    config.selection.minIntradayRangePercent = 0.3;
    config.decision.requireVolumeConfirmation = false;

    zerotick::BacktestEngine engine;
    const zerotick::BacktestResult result =
        engine.run(universe, data, {{"TEST", 0.01}}, config);
    require(result.generatedSignals == 1,
            "the initial qualifying bar should create one signal");
    require(result.filledEntries == 0 && result.unfilledEntries == 1,
            "entry should expire when no trigger occurs within five minutes");
}

void testBacktestUsesStopFirstWhenCandleTouchesBothLevels() {
    TestUniverse universe({"TEST"});
    std::map<std::string, std::vector<zerotick::HistoricalCandle>> data;
    data["TEST"] = {
        makeCandle("2026-11-18T12:00:00+0530",
                   150.0, 151.0, 149.0, 150.0, 100000),
        makeCandle("2026-11-18T12:01:00+0530",
                   150.0, 151.2, 149.9, 150.15, 1000),
        makeCandle("2026-11-18T12:02:00+0530",
                   150.15, 150.18, 150.14, 150.16, 1000),
        makeCandle("2026-11-18T12:03:00+0530",
                   150.16, 151.0, 149.0, 150.0, 1000)
    };
    std::map<std::string, double> tickSizes;
    tickSizes["TEST"] = 0.01;

    zerotick::BacktestConfig config;
    config.selection.minAbsoluteVolume = 1;
    config.selection.minRelativeVolume = 0.0;
    config.selection.minIntradayRangePercent = 0.3;
    config.decision.requireVolumeConfirmation = false;

    zerotick::BacktestEngine engine;
    const zerotick::BacktestResult result =
        engine.run(universe, data, tickSizes, config);
    require(result.trades.size() == 1, "trade should close on the test bar");
    require(result.trades[0].exitReason == "stop_loss",
            "ambiguous bars should assume the stop was hit first");
    require(result.trades[0].grossProfitLoss < 0.0,
            "stop-first handling should record a conservative loss");
}

void testBacktestIgnoresSignalsBeforeEntryWindow() {
    TestUniverse universe({"TEST"});
    std::map<std::string, std::vector<zerotick::HistoricalCandle>> data;
    data["TEST"] = {
        makeCandle("2026-11-18T11:58:00+0530",
                   150.0, 151.0, 149.0, 150.0, 100000),
        makeCandle("2026-11-18T11:59:00+0530",
                   150.0, 150.2, 149.9, 150.15, 1000)
    };

    zerotick::BacktestConfig config;
    config.selection.minAbsoluteVolume = 1;
    config.selection.minRelativeVolume = 0.0;
    config.selection.minIntradayRangePercent = 0.3;
    config.decision.requireVolumeConfirmation = false;

    zerotick::BacktestEngine engine;
    const zerotick::BacktestResult result =
        engine.run(universe, data, {{"TEST", 0.01}}, config);
    require(result.generatedSignals == 0,
            "signals before noon must be excluded from the backtest");
    require(result.trades.empty(),
            "no position should open before the entry window");
}

void testBacktestCancelsEntriesAtOrAfterWindowEnd() {
    TestUniverse universe({"TEST"});
    std::map<std::string, std::vector<zerotick::HistoricalCandle>> data;
    data["TEST"] = {
        makeCandle("2026-11-18T12:58:00+0530",
                   150.0, 151.0, 149.0, 150.0, 100000),
        makeCandle("2026-11-18T12:59:00+0530",
                   150.0, 150.2, 149.9, 150.15, 1000),
        makeCandle("2026-11-18T13:00:00+0530",
                   150.15, 150.3, 150.1, 150.16, 1000)
    };

    zerotick::BacktestConfig config;
    config.selection.minAbsoluteVolume = 1;
    config.selection.minRelativeVolume = 0.0;
    config.selection.minIntradayRangePercent = 0.3;
    config.decision.requireVolumeConfirmation = false;

    zerotick::BacktestEngine engine;
    const zerotick::BacktestResult result =
        engine.run(universe, data, {{"TEST", 0.01}}, config);
    require(result.generatedSignals == 1,
            "a signal may be generated for an entry before the window ends");
    require(result.filledEntries == 0 && result.trades.empty(),
            "an entry at 13:00 must not fill outside the noon window");
    require(result.unfilledEntries == 1,
            "the out-of-window pending entry should be counted as unfilled");
}

void testBacktestShortEntryAndTargetExit() {
    TestUniverse universe({"TEST"});
    std::map<std::string, std::vector<zerotick::HistoricalCandle>> data;
    data["TEST"] = {
        makeCandle("2026-11-18T12:00:00+0530",
                   150.0, 151.0, 149.0, 150.0, 100000),
        makeCandle("2026-11-18T12:01:00+0530",
                   150.0, 150.05, 149.8, 149.85, 1000),
        makeCandle("2026-11-18T12:02:00+0530",
                   149.85, 149.9, 149.7, 149.75, 1000),
        makeCandle("2026-11-18T12:03:00+0530",
                   149.75, 149.8, 149.0, 149.1, 1000)
    };

    zerotick::BacktestConfig config;
    config.selection.minPrice = 100.0;
    config.selection.minAbsoluteVolume = 1;
    config.selection.minRelativeVolume = 0.0;
    config.selection.minIntradayRangePercent = 0.3;
    config.selection.allowShortSelling = true;
    config.decision.allowShortSelling = true;
    config.decision.requireVolumeConfirmation = false;

    zerotick::BacktestEngine engine;
    const zerotick::BacktestResult result =
        engine.run(universe, data, {{"TEST", 0.01}}, config);
    require(result.generatedSignals == 1,
            "downward momentum should generate one short signal");
    require(result.filledEntries == 1,
            "next candle should fill the short stop-limit entry");
    require(result.trades.size() == 1,
            "short target should close the position");
    require(result.trades[0].side == zerotick::PositionSide::Short,
            "backtest trade should retain short direction");
    require(result.trades[0].exitReason == "target",
            "downward target should close the short");
    require(result.trades[0].grossProfitLoss > 0.0,
            "a short target exit should produce positive P&L");
    require(std::abs(
                result.endingCapital -
                (result.initialCapital + result.grossProfitLoss)) < 0.0001,
            "short proceeds and cover cost should reconcile to P&L");
}

void testBacktestShortStopWinsWhenBothLevelsAreTouched() {
    TestUniverse universe({"TEST"});
    std::map<std::string, std::vector<zerotick::HistoricalCandle>> data;
    data["TEST"] = {
        makeCandle("2026-11-18T12:00:00+0530",
                   150.0, 151.0, 149.0, 150.0, 100000),
        makeCandle("2026-11-18T12:01:00+0530",
                   150.0, 150.05, 149.8, 149.85, 1000),
        makeCandle("2026-11-18T12:02:00+0530",
                   149.85, 149.9, 149.7, 149.75, 1000),
        makeCandle("2026-11-18T12:03:00+0530",
                   150.5, 150.6, 149.0, 150.0, 1000)
    };

    zerotick::BacktestConfig config;
    config.selection.minAbsoluteVolume = 1;
    config.selection.minRelativeVolume = 0.0;
    config.selection.minIntradayRangePercent = 0.3;
    config.selection.allowShortSelling = true;
    config.decision.allowShortSelling = true;
    config.decision.requireVolumeConfirmation = false;

    zerotick::BacktestEngine engine;
    const zerotick::BacktestResult result =
        engine.run(universe, data, {{"TEST", 0.01}}, config);
    require(result.trades.size() == 1,
            "short position should close after crossing its stop");
    require(result.trades[0].exitReason == "stop_loss",
            "short stop should win when both levels are touched");
    require(result.trades[0].grossProfitLoss < 0.0,
            "short stop should record a loss");
}

void testFullPipelinePaperMode() {
    zerotick::MarketSnapshot baseline =
        makeQuote(150.0, 200000, 5000, 1000, 149.0, 151.0, 148.0);
    zerotick::MarketSnapshot quote =
        makeQuote(150.15, 200000, 5000, 1000, 149.0, 151.0, 148.0);
    quote.receivedAt = baseline.receivedAt + std::chrono::milliseconds(1);
    FakeKiteApi api(quote);
    TestUniverse universe({"TEST"});
    zerotick::StockEngineRegistry registry;
    zerotick::ThreadSafeQueue<zerotick::MarketSnapshot> quoteUpdates;
    quoteUpdates.push(baseline);
    quoteUpdates.push(quote);
    zerotick::ThreadSafeQueue<zerotick::StockCandidate> selectionToDecision;
    zerotick::ThreadSafeQueue<zerotick::TradeSignal> decisionToHandling;
    zerotick::PortfolioRiskManager portfolioRisk(100000.0);

    zerotick::SelectionConfig selectionConfig;
    selectionConfig.workerCount = 1;
    selectionConfig.minPrice = 100.0;
    selectionConfig.minAbsoluteVolume = 1000;
    selectionConfig.minRelativeVolume = 0.0;
    selectionConfig.maxSpreadPercent = 0.5;
    selectionConfig.minDepthQuantity = 100;
    selectionConfig.minIntradayRangePercent = 0.0;
    selectionConfig.maxPriceChangePercent = 100.0;

    zerotick::DecisionConfig decisionConfig;
    decisionConfig.workerCount = 1;
    decisionConfig.minimumImbalance = 0.0;
    decisionConfig.maximumSpreadPercent = 1.0;
    decisionConfig.requireVolumeConfirmation = false;

    zerotick::HandlingConfig handlingConfig;
    handlingConfig.workerCount = 1;
    handlingConfig.pollInterval = std::chrono::milliseconds(10);
    handlingConfig.liveTrading = false;

    zerotick::StockSelectionEngine selectionEngine(
        quoteUpdates, registry, selectionToDecision, universe, selectionConfig);
    zerotick::TradingDecisionEngine decisionEngine(
        api, registry, selectionToDecision, decisionToHandling, decisionConfig);
    zerotick::TradeHandlingEngine handlingEngine(
        api, registry, decisionToHandling, portfolioRisk, handlingConfig);

    selectionEngine.start();
    decisionEngine.start();
    handlingEngine.start();

    bool released = false;
    for (int i = 0; i < 400 && !released; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (registry.isUnassigned("TEST")) {
            released = true;
        }
    }
    require(released,
            "full pipeline should route TEST through all engines and release it");

    selectionEngine.stop();
    decisionEngine.stop();
    handlingEngine.stop();
}

} // namespace

int main() {
    try {
        testLogLinePersistsToLogsFile();
        testStructuredEventWriterRotatesCompleteJsonLines();
        testNiftyUniverseUsesCurrentNifty100Symbols();
        testRegistryOneEngineAtATime();
        testSelectionSelectsGoodStock();
        testSelectionSelectsShortOnDownwardMomentumWhenEnabled();
        testSelectionKeepsShortSellingDisabledByDefault();
        testSelectionRejectsPriceChangeBelowMinimum();
        testSelectionRejectsLowPrice();
        testSelectionRejectsLowVolume();
        testSelectionWaitsUntilQuoteAvailable();
        testDecisionEmitsSignal();
        testDecisionEmitsShortSignalWithDownwardOrderBookPressure();
        testHandlingPaperModeRoundTrip();
        testQuantityUsesFullEqualSlotBudget();
        testPortfolioRiskManagerLimitsAndDividesSlotsEqually();
        testFreeCashSubtractsExistingUtilization();
        testRuntimeConfigLoadsAlgorithmParameters();
        testRuntimeConfigRejectsUnknownAndInvalidValues();
        testCheckedInRuntimeConfigLoads();
        testBacktestPositionCapIsIndependentByDefault();
        testBacktestReportsInsufficientPerPositionBudget();
        testBacktestUsesNextCandleEntryAndTargetExit();
        testBacktestKeepsEntryPendingForFiveMinutes();
        testBacktestExpiresEntryAfterFiveMinutes();
        testBacktestUsesStopFirstWhenCandleTouchesBothLevels();
        testBacktestIgnoresSignalsBeforeEntryWindow();
        testBacktestCancelsEntriesAtOrAfterWindowEnd();
        testBacktestShortEntryAndTargetExit();
        testBacktestShortStopWinsWhenBothLevelsAreTouched();
        testFullPipelinePaperMode();
    } catch (const std::exception& exception) {
        std::cerr << "Engine test failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "All engine tests passed\n";
    return 0;
}



