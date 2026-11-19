#include "zerotick/Worker.h"
#include "zerotick/Log.h"
#include "zerotick/Risk.h"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace zerotick {
namespace {

void logMarketStatus(
    const MarketSnapshot& snapshot,
    const StrategyDiagnostics& indicators,
    const char* decision,
    WorkerState state) {
    const long long quoteAgeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - snapshot.receivedAt).count();

    std::ostringstream line;
    line << std::fixed
         << "[worker] symbol=" << snapshot.symbol
         << " state=" << toString(state)
         << " last=" << std::setprecision(2) << snapshot.lastPrice
         << " bid=" << snapshot.bidPrice
         << " ask=" << snapshot.askPrice
         << " bid_depth_qty=" << snapshot.bidQuantity
         << " ask_depth_qty=" << snapshot.askQuantity
         << " weighted_imbalance=" << std::setprecision(3)
         << indicators.orderImbalance
         << " spread_pct=" << std::setprecision(3)
         << indicators.spreadPercent
         << " price_change_pct=" << indicators.priceChangePercent
         << " entry_trigger=" << indicators.entryTriggerPrice
         << " entry_limit=" << indicators.entryLimitPrice
         << " confirmations=" << indicators.consecutiveConfirmations
         << " signal=" << decision
         << " reason=\"" << indicators.reason << '"'
         << " quote_age_ms=" << quoteAgeMs;
    logLine(line.str());
}

double priceTick(const MarketSnapshot& snapshot) {
    double smallestStep = 0.0;
    for (std::size_t level = 1; level < 5; ++level) {
        const double steps[] = {
            snapshot.bidLevels[level - 1].price -
                snapshot.bidLevels[level].price,
            snapshot.askLevels[level].price -
                snapshot.askLevels[level - 1].price
        };
        for (double step : steps) {
            const double normalizedStep = std::round(step * 100.0) / 100.0;
            if (normalizedStep > 0.0 &&
                (smallestStep == 0.0 || normalizedStep < smallestStep)) {
                smallestStep = normalizedStep;
            }
        }
    }
    return smallestStep > 0.0 ? smallestStep : 0.01;
}

} // namespace

Worker::Worker(
    std::string symbol,
    KiteApi& api,
    std::unique_ptr<Strategy> strategy,
    std::chrono::milliseconds pollInterval,
    std::atomic_bool& stopRequested,
    bool liveTrading,
    PortfolioRiskManager& portfolioRisk)
    : symbol_(std::move(symbol)),
      api_(api),
      strategy_(std::move(strategy)),
      pollInterval_(pollInterval),
      stopRequested_(stopRequested),
      liveTrading_(liveTrading),
      portfolioRisk_(portfolioRisk) {}

void Worker::run() {
    start();

    while (!stopRequested_) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            const WorkerState current = stateMachine_.state();
            if (current == WorkerState::Stopped ||
                current == WorkerState::Error) {
                break;
            }
        }

        if (!api_.isHealthy()) {
            logLine("[worker] symbol=" + symbol_ +
                    " error=Kite_API_not_authenticated");
            std::this_thread::sleep_for(pollInterval_);
            continue;
        }

        try {
            monitor();
        } catch (const std::exception& exception) {
            const WorkerState current = state();
            logLine("[worker] symbol=" + symbol_ +
                    " operation_error=\"" + exception.what() +
                    "\" state=" + toString(current));
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (current == WorkerState::Monitoring ||
                (current == WorkerState::EntryPending &&
                 entryOrderId_.empty()) ||
                (current == WorkerState::ExitPending &&
                 exitOrderId_.empty() &&
                 exitRetryAfter_.time_since_epoch().count() == 0)) {
                stateMachine_.transition(WorkerEvent::Failure);
            }
        }
        std::this_thread::sleep_for(pollInterval_);
    }

    stop();
}

void Worker::start() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    stateMachine_.transition(WorkerEvent::Start);
}

void Worker::monitor() {
    std::lock_guard<std::mutex> lock(stateMutex_);

    switch (stateMachine_.state()) {
        case WorkerState::Monitoring:
            handleEntry();
            break;
        case WorkerState::EntryPending:
            handleEntryPending();
            break;
        case WorkerState::PositionOpen:
            handlePosition();
            break;
        case WorkerState::ExitPending:
            handleExitPending();
            break;
        case WorkerState::Cooldown:
            stateMachine_.transition(WorkerEvent::PollTimeout);
            break;
        default:
            break;
    }
}

void Worker::handleEntry() {
    const MarketSnapshot snapshot = api_.getQuote(symbol_);
    if (snapshot.receivedAt <= lastStrategySnapshotAt_) {
        return;
    }
    lastStrategySnapshotAt_ = snapshot.receivedAt;

    const StrategyDecision decision = strategy_->evaluateEntry(snapshot);
    const StrategyDiagnostics diagnostics = strategy_->diagnostics();
    logMarketStatus(
        snapshot,
        diagnostics,
        decision == StrategyDecision::Enter ? "ENTRY" : "NO_ENTRY",
        stateMachine_.state());

    if (decision != StrategyDecision::Enter) {
        return;
    }
    if (!liveTrading_) {
        logLine("[worker] symbol=" + symbol_ +
                " signal=ENTRY live_trading=disabled order=not_submitted");
        return;
    }

    std::lock_guard<std::mutex> entryLock(
        portfolioRisk_.entryWorkflowMutex());
    if (portfolioRisk_.activeReservations() >=
        kMaximumLivePositions) {
        stateMachine_.transition(WorkerEvent::EntrySignal);
        stateMachine_.transition(WorkerEvent::EntryRejected);
        logLine(
            "[worker] symbol=" + symbol_ +
            " order=not_submitted reason=\"global live-position limit "
            "reached\" active_positions=" +
            std::to_string(portfolioRisk_.activeReservations()));
        return;
    }

    stateMachine_.transition(WorkerEvent::EntrySignal);
    const CashAvailability funds = api_.cashAvailability();
    double slotBudget = 0.0;
    if (!portfolioRisk_.reserveSlot(
            symbol_, funds.freeCash, slotBudget)) {
        stateMachine_.transition(WorkerEvent::EntryRejected);
        logLine(
            "[worker] symbol=" + symbol_ +
            " order=not_submitted reason=\"global live-position limit "
            "reached or symbol already reserved\" active_positions=" +
            std::to_string(portfolioRisk_.activeReservations()));
        return;
    }

    double oneShareMargin = 0.0;
    double estimatedOrderFunds = 0.0;
    try {
        oneShareMargin = api_.estimateMisOrderFunds(
            symbol_,
            1,
            diagnostics.entryTriggerPrice,
            diagnostics.entryLimitPrice);
        if (!std::isfinite(oneShareMargin) || oneShareMargin <= 0.0) {
            throw std::runtime_error(
                "Kite returned an invalid one-share MIS margin estimate");
        }
        quantity_ =
            calculateQuantityWithinBudget(slotBudget, oneShareMargin);
        if (quantity_ > 0) {
            estimatedOrderFunds = quantity_ == 1
                ? oneShareMargin
                : api_.estimateMisOrderFunds(
                    symbol_,
                    quantity_,
                    diagnostics.entryTriggerPrice,
                    diagnostics.entryLimitPrice);
            if (!std::isfinite(estimatedOrderFunds) ||
                estimatedOrderFunds <= 0.0) {
                throw std::runtime_error(
                    "Kite returned an invalid MIS margin estimate");
            }
            for (std::size_t attempt = 0;
                 estimatedOrderFunds > slotBudget && attempt < 8;
                 ++attempt) {
                int adjustedQuantity = calculateQuantityWithinBudget(
                    slotBudget,
                    estimatedOrderFunds / quantity_);
                if (adjustedQuantity >= quantity_) {
                    adjustedQuantity = quantity_ - 1;
                }
                quantity_ = adjustedQuantity;
                if (quantity_ <= 0) {
                    break;
                }
                estimatedOrderFunds = api_.estimateMisOrderFunds(
                    symbol_,
                    quantity_,
                    diagnostics.entryTriggerPrice,
                    diagnostics.entryLimitPrice);
                if (!std::isfinite(estimatedOrderFunds) ||
                    estimatedOrderFunds <= 0.0) {
                    throw std::runtime_error(
                        "Kite returned an invalid MIS margin estimate");
                }
            }
        }
    } catch (const std::exception& exception) {
        quantity_ = 0;
        portfolioRisk_.releaseSlot(symbol_);
        stateMachine_.transition(WorkerEvent::EntryRejected);
        logLine(
            "[worker] symbol=" + symbol_ +
            " order=not_submitted reason=\"MIS margin estimate failed: " +
            exception.what() + '"');
        return;
    }

    if (quantity_ <= 0 || estimatedOrderFunds > slotBudget) {
        quantity_ = 0;
        portfolioRisk_.releaseSlot(symbol_);
        stateMachine_.transition(WorkerEvent::EntryRejected);
        std::ostringstream message;
        message << std::fixed << std::setprecision(2)
                << "[worker] symbol=" << symbol_
                << " order=not_submitted reason=\"equal portfolio slot "
                   "cannot cover one MIS share\" free_cash="
                << funds.freeCash
                << " slot_budget=" << slotBudget
                << " one_share_margin_and_charges=" << oneShareMargin
                << " entry_limit=" << diagnostics.entryLimitPrice;
        logLine(message.str());
        return;
    }

    OrderResult order;
    try {
        order = api_.placeStopLimitEntry(
            symbol_,
            quantity_,
            diagnostics.entryTriggerPrice,
            diagnostics.entryLimitPrice);
    } catch (const KiteHttpError& exception) {
        if (exception.statusCode() >= 400 &&
            exception.statusCode() < 500) {
            quantity_ = 0;
            portfolioRisk_.releaseSlot(symbol_);
            stateMachine_.transition(WorkerEvent::EntryRejected);
            logLine("[worker] symbol=" + symbol_ +
                    " order=rejected http_status=" +
                    std::to_string(exception.statusCode()) +
                    " reason=\"" + exception.what() + '"');
            return;
        }
        throw;
    }
    if (!order.accepted || order.orderId.empty()) {
        quantity_ = 0;
        portfolioRisk_.releaseSlot(symbol_);
        stateMachine_.transition(WorkerEvent::EntryRejected);
        logLine("[worker] symbol=" + symbol_ +
                " order=not_submitted reason=\"" + order.message + '"');
        return;
    }

    entryOrderId_ = order.orderId;
    entryOrderSubmittedAt_ = std::chrono::steady_clock::now();
    entryCancelRequested_ = false;
    logLine("[worker] symbol=" + symbol_ +
            " order=stop_limit_submitted order_id=" + entryOrderId_ +
            " quantity=" + std::to_string(quantity_) +
            " free_cash=" + std::to_string(funds.freeCash) +
            " equal_slot_budget=" + std::to_string(slotBudget) +
            " estimated_mis_funds=" +
            std::to_string(estimatedOrderFunds) +
            " estimated_notional=" +
            std::to_string(
                quantity_ * diagnostics.entryLimitPrice) +
            " product=MIS");
}

void Worker::handleEntryPending() {
    if (entryOrderId_.empty()) {
        throw std::runtime_error(
            "Entry order outcome is unknown; inspect Kite orders before restart");
    }

    const BrokerOrderStatus status = api_.getOrderStatus(entryOrderId_);
    if (status.terminal) {
        entryOrderId_.clear();
        if (status.filledQuantity > 0 && status.averagePrice > 0.0) {
            quantity_ = status.filledQuantity;
            entryPrice_ = status.averagePrice;
            stateMachine_.transition(WorkerEvent::EntryAccepted);
            logLine("[worker] symbol=" + symbol_ +
                    " entry=filled status=" + status.status +
                    " quantity=" + std::to_string(quantity_) +
                    " average_price=" + std::to_string(entryPrice_));
        } else {
            if (status.filledQuantity > 0) {
                throw std::runtime_error(
                    "Kite reported an entry fill without a valid average price");
            }
            quantity_ = 0;
            portfolioRisk_.releaseSlot(symbol_);
            stateMachine_.transition(WorkerEvent::EntryRejected);
            logLine("[worker] symbol=" + symbol_ +
                    " entry=not_filled status=" + status.status);
        }
        return;
    }

    if (!entryCancelRequested_ &&
        std::chrono::steady_clock::now() - entryOrderSubmittedAt_ >=
            std::chrono::seconds(30)) {
        api_.cancelOrder(entryOrderId_);
        entryCancelRequested_ = true;
        logLine("[worker] symbol=" + symbol_ +
                " entry=cancel_requested order_id=" + entryOrderId_ +
                " reason=30_second_timeout");
    }
}

void Worker::handlePosition() {
    const MarketSnapshot snapshot = api_.getQuote(symbol_);
    if (snapshot.receivedAt <= lastStrategySnapshotAt_) {
        return;
    }
    lastStrategySnapshotAt_ = snapshot.receivedAt;

    const StrategyDecision decision =
        strategy_->evaluateExit(snapshot, entryPrice_);
    const StrategyDiagnostics diagnostics = strategy_->diagnostics();
    logMarketStatus(
        snapshot,
        diagnostics,
        decision == StrategyDecision::Exit ? "EXIT" : "HOLD",
        stateMachine_.state());

    if (decision == StrategyDecision::Exit) {
        submitExit(snapshot, diagnostics.stopLossTriggered);
    }
}

void Worker::submitExit(
    const MarketSnapshot& snapshot,
    bool stopLoss) {
    if (!liveTrading_) {
        return;
    }
    if (stateMachine_.state() == WorkerState::PositionOpen) {
        stateMachine_.transition(WorkerEvent::ExitSignal);
    }
    exitIsStopLoss_ = stopLoss;
    handleExit(snapshot);
}

void Worker::handleExit(const MarketSnapshot& snapshot) {
    const double tick = priceTick(snapshot);
    const double targetPrice =
        std::ceil(entryPrice_ * 1.002 / tick - 1e-9) * tick;
    const double limitPrice = exitIsStopLoss_
        ? (snapshot.bidPrice > 0.0 ? snapshot.bidPrice : snapshot.lastPrice)
        : targetPrice;
    if (limitPrice <= 0.0) {
        throw std::runtime_error(
            "Cannot submit limit exit without a valid market price");
    }

    const OrderResult order =
        api_.placeLimitExit(symbol_, quantity_, limitPrice);
    if (!order.accepted || order.orderId.empty()) {
        throw std::runtime_error(
            "Kite did not confirm exit order submission: " + order.message);
    }

    exitOrderId_ = order.orderId;
    exitOrderQuantity_ = quantity_;
    exitOrderSubmittedAt_ = std::chrono::steady_clock::now();
    exitCancelRequested_ = false;
    logLine("[worker] symbol=" + symbol_ +
            " exit=limit_submitted order_id=" + exitOrderId_ +
            " quantity=" + std::to_string(exitOrderQuantity_) +
            " limit_price=" + std::to_string(limitPrice) +
            " reason=" + (exitIsStopLoss_ ? "stop_loss" : "target"));
}

void Worker::handleExitPending() {
    if (exitOrderId_.empty()) {
        if (exitRetryAfter_.time_since_epoch().count() == 0) {
            throw std::runtime_error(
                "Exit order outcome is unknown; inspect Kite orders immediately");
        }
        if (std::chrono::steady_clock::now() < exitRetryAfter_) {
            return;
        }
        const MarketSnapshot snapshot = api_.getQuote(symbol_);
        exitRetryAfter_ = std::chrono::steady_clock::time_point();
        handleExit(snapshot);
        return;
    }

    const BrokerOrderStatus status = api_.getOrderStatus(exitOrderId_);
    if (status.terminal) {
        if (status.filledQuantity < 0 ||
            status.filledQuantity > exitOrderQuantity_) {
            throw std::runtime_error(
                "Kite returned an invalid exit fill quantity");
        }
        quantity_ -= status.filledQuantity;
        logLine("[worker] symbol=" + symbol_ +
                " exit_order_status=" + status.status +
                " filled=" + std::to_string(status.filledQuantity) +
                " remaining_position=" + std::to_string(quantity_));
        exitOrderId_.clear();
        exitCancelRequested_ = false;

        if (quantity_ == 0) {
            entryPrice_ = 0.0;
            portfolioRisk_.releaseSlot(symbol_);
            stateMachine_.transition(WorkerEvent::ExitCompleted);
            logLine("[worker] symbol=" + symbol_ +
                    " position=closed state=" +
                    toString(stateMachine_.state()));
            return;
        }

        exitRetryAfter_ =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        return;
    }

    if (!exitCancelRequested_ &&
        std::chrono::steady_clock::now() - exitOrderSubmittedAt_ >=
            std::chrono::seconds(5)) {
        api_.cancelOrder(exitOrderId_);
        exitCancelRequested_ = true;
        logLine("[worker] symbol=" + symbol_ +
                " exit=cancel_requested order_id=" + exitOrderId_ +
                " reason=limit_not_filled_after_5_seconds");
    }
}

void Worker::stop() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const WorkerState current = stateMachine_.state();
    if (current == WorkerState::EntryPending && !entryOrderId_.empty()) {
        try {
            api_.cancelOrder(entryOrderId_);
            logLine("[worker] symbol=" + symbol_ +
                    " shutdown=entry_cancel_requested order_id=" +
                    entryOrderId_);
        } catch (const std::exception& exception) {
            logLine("[worker] symbol=" + symbol_ +
                    " shutdown=entry_cancel_failed order_id=" +
                    entryOrderId_ + " reason=\"" + exception.what() + '"');
        }
    } else if (current == WorkerState::PositionOpen && liveTrading_) {
        try {
            stateMachine_.transition(WorkerEvent::ExitSignal);
            exitIsStopLoss_ = true;
            handleExit(api_.getQuote(symbol_));
            logLine("[worker] symbol=" + symbol_ +
                    " shutdown=exit_order_submitted; verify fill in Kite");
        } catch (const std::exception& exception) {
            logLine("[worker] symbol=" + symbol_ +
                    " shutdown=position_may_remain_open reason=\"" +
                    exception.what() + '"');
        }
    } else if ((current == WorkerState::PositionOpen ||
                current == WorkerState::ExitPending ||
                current == WorkerState::Error) &&
               quantity_ > 0) {
        logLine("[worker] symbol=" + symbol_ +
                " shutdown=position_or_order_may_remain_open; verify Kite");
    }

    stateMachine_.transition(WorkerEvent::Stop);
}

WorkerState Worker::state() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return stateMachine_.state();
}

const std::string& Worker::symbol() const noexcept {
    return symbol_;
}

} // namespace zerotick
