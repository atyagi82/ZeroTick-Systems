#include "zerotick/Strategy.h"
#include "zerotick/Risk.h"
#include "zerotick/Worker.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

zerotick::MarketSnapshot makeBook(double lastPrice);

class FakeKiteApi : public zerotick::KiteApi {
public:
    explicit FakeKiteApi(
        std::atomic_bool& stopRequested,
        bool stopAtEntrySignal = false,
        bool rejectEntry = false,
        bool repeatFirstQuote = false)
        : stopRequested_(stopRequested),
          stopAtEntrySignal_(stopAtEntrySignal),
          rejectEntry_(rejectEntry),
          repeatFirstQuote_(repeatFirstQuote) {}

    zerotick::MarketSnapshot getQuote(const std::string&) override {
        const std::size_t sample = quoteCount_++;
        if (stopAtEntrySignal_ && sample >= 3) {
            stopRequested_.store(true);
        }
        if (repeatFirstQuote_) {
            if (!hasRepeatedQuote_) {
                repeatedQuote_ = makeBook(100.00);
                hasRepeatedQuote_ = true;
            }
            return repeatedQuote_;
        }
        if (sample < 4) {
            return makeBook(100.00 + sample * 0.01);
        }
        return makeBook(100.31);
    }

    void refreshQuotes(const std::vector<std::string>&) override {}

    void ensureNoOpenMisExposure(
        const std::vector<std::string>&) override {}

    zerotick::CashAvailability cashAvailability() override {
        ++freeCashRequests_;
        return {10000.0, 0.0, 0.0, 10000.0, 0.0, 0.0, 10000.0};
    }

    double estimateMisOrderFunds(
        const std::string&,
        int quantity,
        double,
        double) override {
        ++marginEstimateRequests_;
        return quantity * 50.0 + 0.10;
    }

    zerotick::OrderResult placeStopLimitEntry(
        const std::string&,
        int quantity,
        double triggerPrice,
        double limitPrice) override {
        entryQuantity = quantity;
        entryTriggerPrice = triggerPrice;
        entryLimitPrice = limitPrice;
        if (rejectEntry_) {
            stopRequested_.store(true);
            throw zerotick::KiteHttpError(
                403,
                "Kite order permission denied");
        }
        return {true, "entry-order", "submitted"};
    }

    zerotick::OrderResult placeLimitExit(
        const std::string&,
        int quantity,
        double limitPrice) override {
        exitQuantity = quantity;
        exitLimitPrice = limitPrice;
        return {true, "exit-order", "submitted"};
    }

    zerotick::BrokerOrderStatus getOrderStatus(
        const std::string&) override {
        if (orderStatusCount_++ == 0) {
            return {"COMPLETE", entryQuantity, 100.05, true};
        }
        stopRequested_.store(true);
        return {"COMPLETE", exitQuantity, exitLimitPrice, true};
    }

    void cancelOrder(const std::string&) override {}

    bool isHealthy() const override {
        return true;
    }

    int entryQuantity = 0;
    int exitQuantity = 0;
    int freeCashRequests_ = 0;
    int marginEstimateRequests_ = 0;
    double entryTriggerPrice = 0.0;
    double entryLimitPrice = 0.0;
    double exitLimitPrice = 0.0;

private:
    std::atomic_bool& stopRequested_;
    bool stopAtEntrySignal_;
    bool rejectEntry_;
    bool repeatFirstQuote_;
    bool hasRepeatedQuote_ = false;
    zerotick::MarketSnapshot repeatedQuote_;
    std::size_t quoteCount_ = 0;
    std::size_t orderStatusCount_ = 0;
};

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
    snapshot.receivedAt = std::chrono::steady_clock::now();

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

void testEntryNeedsTwoConfirmedSnapshots() {
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
            zerotick::StrategyDecision::Enter,
        "second consecutive qualifying snapshot should trigger an entry");
    const zerotick::StrategyDiagnostics diagnostics = strategy.diagnostics();
    require(
        diagnostics.entryTriggerPrice > 100.02 &&
            diagnostics.entryLimitPrice > diagnostics.entryTriggerPrice,
        "stop-limit trigger and limit should step above the current market");
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

void testModestPositiveImbalanceCanQualify() {
    zerotick::OrderImbalanceStrategy strategy;
    for (std::size_t level = 0; level < 5; ++level) {
        zerotick::MarketSnapshot snapshot = makeBook(100.00 + level * 0.01);
        snapshot.bidQuantity = 0;
        snapshot.askQuantity = 0;
        for (zerotick::OrderBookLevel& bid : snapshot.bidLevels) {
            bid.quantity = 1110;
            snapshot.bidQuantity += bid.quantity;
        }
        for (zerotick::OrderBookLevel& ask : snapshot.askLevels) {
            ask.quantity = 1000;
            snapshot.askQuantity += ask.quantity;
        }
        const zerotick::StrategyDecision decision =
            strategy.evaluateEntry(snapshot);
        if (level < 2) {
            require(
                decision == zerotick::StrategyDecision::None,
                "modest imbalance needs two consecutive confirmations");
        } else if (level == 2) {
            require(
                decision == zerotick::StrategyDecision::Enter,
                "imbalance just above +0.05 should qualify after confirmation");
        } else {
            require(
                decision == zerotick::StrategyDecision::None,
                "one confirmation run should emit only one entry signal");
        }
    }
    require(
        strategy.diagnostics().orderImbalance > 0.05 &&
            strategy.diagnostics().orderImbalance < 0.06,
        "test signal should be only slightly above the new imbalance threshold");
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
        diagnostics.orderImbalance < 0.05,
        "weighted imbalance should reflect ask-side pressure");
    require(
        diagnostics.consecutiveConfirmations == 0,
        "failed imbalance filter must reset confirmation count");
    require(
        diagnostics.reason == "weighted imbalance below +0.05",
        "below-threshold imbalance should be reported as the entry reason");
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

void testQuantityUsesFullEqualSlotBudget() {
    require(
        zerotick::calculateQuantityWithinBudget(1000.0, 50.10) == 19,
        "quantity should use the MIS margin requirement within one slot");
    require(
        zerotick::calculateQuantityWithinBudget(10.0, 17.90) == 0,
        "an equal slot below one-share margin must not produce a quantity");
}

void testPortfolioRiskManagerLimitsAndDividesSlotsEqually() {
    zerotick::PortfolioRiskManager risk(1000.0);
    for (int index = 0; index < 10; ++index) {
        double slotBudget = 0.0;
        require(
            risk.reserveSlot(
                "SYMBOL" + std::to_string(index), 1000.0, slotBudget),
            "each of ten symbols should reserve one portfolio slot");
        require(
            std::abs(slotBudget - 100.0) < 0.0001,
            "portfolio cash should be divided equally across ten slots");
    }

    double extraSlotBudget = 0.0;
    require(
        !risk.reserveSlot("EXTRA", 1000.0, extraSlotBudget),
        "a position beyond the global ten-slot limit must be blocked");
    risk.releaseSlot("SYMBOL0");
    require(
        risk.reserveSlot("EXTRA", 1000.0, extraSlotBudget),
        "closing a position should release its portfolio slot");
}

void testFreeCashSubtractsExistingUtilization() {
    require(
        std::abs(
            zerotick::calculateFreeCash(
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

void testLiveWorkerSizesAndTracksStopLimitOrders() {
    std::atomic_bool stopRequested{false};
    FakeKiteApi api(stopRequested);
    zerotick::PortfolioRiskManager portfolioRisk(10000.0);
    zerotick::Worker worker(
        "IDEA",
        api,
        std::make_unique<zerotick::OrderImbalanceStrategy>(),
        std::chrono::milliseconds(1),
        stopRequested,
        true,
        portfolioRisk);
    worker.run();

    require(api.freeCashRequests_ == 1, "live entry should read free cash");
    require(
        api.entryQuantity == 19,
        "entry should size from one equal slot using MIS margin");
    require(
        api.marginEstimateRequests_ == 2,
        "entry should verify the estimated margin for its final quantity");
    require(
        std::abs(api.entryTriggerPrice - 100.03) < 0.001 &&
            std::abs(api.entryLimitPrice - 100.04) < 0.001,
        "entry should submit the strategy-derived stop-limit prices");
    require(
        api.exitQuantity == api.entryQuantity,
        "exit should use the actual filled quantity");
    require(
        std::abs(api.exitLimitPrice - 100.26) < 0.001,
        "target exit should use a tick-rounded target limit");
    require(
        portfolioRisk.activeReservations() == 0,
        "a completed round trip should release its portfolio slot");
}

void testWorkerDoesNotSubmitOrdersWithoutRuntimeOptIn() {
    std::atomic_bool stopRequested{false};
    FakeKiteApi api(stopRequested, true);
    zerotick::PortfolioRiskManager portfolioRisk(0.0);
    zerotick::Worker worker(
        "IDEA",
        api,
        std::make_unique<zerotick::OrderImbalanceStrategy>(),
        std::chrono::milliseconds(1),
        stopRequested,
        false,
        portfolioRisk);
    worker.run();

    require(api.freeCashRequests_ == 0, "data-only mode must not check funds");
    require(api.entryQuantity == 0, "data-only mode must not submit an entry");
    require(api.exitQuantity == 0, "data-only mode must not submit an exit");
}

void testWorkerDoesNotCountRepeatedQuoteSnapshots() {
    std::atomic_bool stopRequested{false};
    FakeKiteApi api(stopRequested, true, false, true);
    zerotick::PortfolioRiskManager portfolioRisk(10000.0);
    zerotick::Worker worker(
        "YESBANK",
        api,
        std::make_unique<zerotick::OrderImbalanceStrategy>(),
        std::chrono::milliseconds(1),
        stopRequested,
        true,
        portfolioRisk);
    worker.run();

    require(
        api.freeCashRequests_ == 0,
        "re-reading one cached quote must not satisfy snapshot confirmations");
}

void testDefinitiveKiteRejectionDoesNotLeaveUnknownPositionState() {
    std::atomic_bool stopRequested{false};
    FakeKiteApi api(stopRequested, false, true);
    zerotick::PortfolioRiskManager portfolioRisk(10000.0);
    zerotick::Worker worker(
        "IDEA",
        api,
        std::make_unique<zerotick::OrderImbalanceStrategy>(),
        std::chrono::milliseconds(1),
        stopRequested,
        true,
        portfolioRisk);
    worker.run();

    require(
        worker.state() == zerotick::WorkerState::Stopped,
        "a definite HTTP rejection should not be treated as an unknown fill");
    require(
        api.exitQuantity == 0,
        "a rejected entry must not cause an exit order");
}

} // namespace

int main() {
    try {
        testEntryNeedsTwoConfirmedSnapshots();
        testWideSpreadBlocksEntry();
        testModestPositiveImbalanceCanQualify();
        testWeakImbalanceBlocksEntryAndResetsConfirmation();
        testFallingPriceResetsConfirmation();
        testTargetAndStopExits();
        testQuantityUsesFullEqualSlotBudget();
        testPortfolioRiskManagerLimitsAndDividesSlotsEqually();
        testFreeCashSubtractsExistingUtilization();
        testLiveWorkerSizesAndTracksStopLimitOrders();
        testWorkerDoesNotSubmitOrdersWithoutRuntimeOptIn();
        testWorkerDoesNotCountRepeatedQuoteSnapshots();
        testDefinitiveKiteRejectionDoesNotLeaveUnknownPositionState();
    } catch (const std::exception& exception) {
        std::cerr << "Strategy test failed: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
