#include "zerotick/Strategy.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

zerotick::MarketSnapshot makeBook(double lastPrice) {
    zerotick::MarketSnapshot snapshot;
    snapshot.symbol = "TEST";
    snapshot.lastPrice = lastPrice;
    snapshot.bidPrice = 99.99;
    snapshot.askPrice = 100.01;

    for (std::size_t level = 0; level < 5; ++level) {
        snapshot.bidLevels[level].price = 99.99 - level * 0.01;
        snapshot.bidLevels[level].quantity = 1000;
        snapshot.askLevels[level].price = 100.01 + level * 0.01;
        snapshot.askLevels[level].quantity = 100;
        snapshot.bidQuantity += snapshot.bidLevels[level].quantity;
        snapshot.askQuantity += snapshot.askLevels[level].quantity;
    }
    return snapshot;
}

void testEntryNeedsThreeConfirmedSnapshots() {
    zerotick::OrderImbalanceStrategy strategy;
    require(
        strategy.evaluateEntry(makeBook(100.00)) ==
            zerotick::StrategyDecision::None,
        "first snapshot must only establish a price baseline");
    require(
        strategy.evaluateEntry(makeBook(100.01)) ==
            zerotick::StrategyDecision::None,
        "first qualifying snapshot must not trigger an entry");
    require(
        strategy.evaluateEntry(makeBook(100.02)) ==
            zerotick::StrategyDecision::None,
        "second qualifying snapshot must not trigger an entry");
    require(
        strategy.evaluateEntry(makeBook(100.03)) ==
            zerotick::StrategyDecision::Enter,
        "third consecutive qualifying snapshot should trigger an entry");
}

void testWideSpreadBlocksEntry() {
    zerotick::OrderImbalanceStrategy strategy;
    zerotick::MarketSnapshot snapshot = makeBook(100.00);
    snapshot.bidPrice = 99.70;
    snapshot.askPrice = 100.30;

    require(
        strategy.evaluateEntry(snapshot) ==
            zerotick::StrategyDecision::None,
        "wide spread should block entry");
    require(
        strategy.diagnostics().reason == "spread exceeds 0.10% limit",
        "wide spread should be reported as the entry reason");
}

void testWeakImbalanceBlocksEntryAndResetsConfirmation() {
    zerotick::OrderImbalanceStrategy strategy;
    strategy.evaluateEntry(makeBook(100.00));
    strategy.evaluateEntry(makeBook(100.01));

    zerotick::MarketSnapshot snapshot = makeBook(100.02);
    snapshot.bidQuantity = 0;
    snapshot.askQuantity = 0;
    for (std::size_t level = 0; level < 5; ++level) {
        snapshot.bidLevels[level].quantity = 100;
        snapshot.askLevels[level].quantity = 1000;
        snapshot.bidQuantity += snapshot.bidLevels[level].quantity;
        snapshot.askQuantity += snapshot.askLevels[level].quantity;
    }

    require(
        strategy.evaluateEntry(snapshot) ==
            zerotick::StrategyDecision::None,
        "weak weighted imbalance should block entry");
    const zerotick::StrategyDiagnostics diagnostics = strategy.diagnostics();
    require(
        diagnostics.orderImbalance < 0.25,
        "weighted imbalance should reflect ask-side pressure");
    require(
        diagnostics.consecutiveConfirmations == 0,
        "failed imbalance filter must reset confirmation count");
}

void testFallingPriceResetsConfirmation() {
    zerotick::OrderImbalanceStrategy strategy;
    strategy.evaluateEntry(makeBook(100.00));
    strategy.evaluateEntry(makeBook(100.01));

    require(
        strategy.evaluateEntry(makeBook(100.00)) ==
            zerotick::StrategyDecision::None,
        "falling price must not trigger entry");
    require(
        strategy.diagnostics().consecutiveConfirmations == 0,
        "falling price must reset confirmation count");
}

void testTargetAndStopExits() {
    zerotick::OrderImbalanceStrategy targetStrategy;
    require(
        targetStrategy.evaluateExit(makeBook(100.20), 100.00) ==
            zerotick::StrategyDecision::Exit,
        "price gain at the 0.20% target should trigger exit");
    require(
        targetStrategy.diagnostics().reason ==
            "0.20% profit target reached",
        "target exit reason should be reported");

    zerotick::OrderImbalanceStrategy stopStrategy;
    require(
        stopStrategy.evaluateExit(makeBook(99.60), 100.00) ==
            zerotick::StrategyDecision::Exit,
        "price loss at the 0.40% stop should trigger exit");
    require(
        stopStrategy.diagnostics().reason == "0.40% stop loss reached",
        "stop exit reason should be reported");
}

} // namespace

int main() {
    try {
        testEntryNeedsThreeConfirmedSnapshots();
        testWideSpreadBlocksEntry();
        testWeakImbalanceBlocksEntryAndResetsConfirmation();
        testFallingPriceResetsConfirmation();
        testTargetAndStopExits();
    } catch (const std::exception& exception) {
        std::cerr << "Strategy test failed: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
