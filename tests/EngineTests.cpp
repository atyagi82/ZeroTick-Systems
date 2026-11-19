#include "zerotick/EngineTypes.h"
#include "zerotick/StockSelectionEngine.h"
#include "zerotick/TradingDecisionEngine.h"
#include "zerotick/TradeHandlingEngine.h"
#include "zerotick/Risk.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
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

class FakeKiteApi : public zerotick::KiteApi {
public:
    explicit FakeKiteApi(const zerotick::MarketSnapshot& quote) : quote_(quote) {}

    zerotick::MarketSnapshot getQuote(const std::string&) override { return quote_; }
    std::vector<zerotick::MarketSnapshot> refreshQuotes(
        const std::vector<std::string>&) override {
        return {};
    }
    void ensureNoOpenMisExposure(const std::vector<std::string>&) override {}
    zerotick::CashAvailability cashAvailability() override {
        return {100000.0, 0.0, 0.0, 100000.0, 0.0, 0.0, 100000.0};
    }
    zerotick::OrderResult placeStopLimitEntry(const std::string&, int, double,
                                              double) override {
        return {true, "order", "submitted"};
    }
    zerotick::OrderResult placeLimitExit(const std::string&, int, double) override {
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
    require(signal.quantity > 0, "signal quantity set");

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
    decisionConfig.pollInterval = std::chrono::milliseconds(10);
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
        testNiftyUniverseUsesCurrentNifty100Symbols();
        testRegistryOneEngineAtATime();
        testSelectionSelectsGoodStock();
        testSelectionRejectsPriceChangeBelowMinimum();
        testSelectionRejectsLowPrice();
        testSelectionRejectsLowVolume();
        testSelectionWaitsUntilQuoteAvailable();
        testDecisionEmitsSignal();
        testHandlingPaperModeRoundTrip();
        testQuantityUsesFullEqualSlotBudget();
        testPortfolioRiskManagerLimitsAndDividesSlotsEqually();
        testFreeCashSubtractsExistingUtilization();
        testFullPipelinePaperMode();
    } catch (const std::exception& exception) {
        std::cerr << "Engine test failed: " << exception.what() << '\n';
        return 1;
    }
    std::cout << "All engine tests passed\n";
    return 0;
}



