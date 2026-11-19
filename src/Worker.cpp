#include "zerotick/Worker.h"
#include "zerotick/Log.h"

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
         << " confirmations=" << indicators.consecutiveConfirmations
         << " signal=" << decision
         << " reason=\"" << indicators.reason << '"'
         << " quote_age_ms=" << quoteAgeMs;
    logLine(line.str());
}

} // namespace

Worker::Worker(
    std::string symbol,
    KiteApi& api,
    std::unique_ptr<Strategy> strategy,
    std::chrono::milliseconds pollInterval,
    std::atomic_bool& stopRequested)
    : symbol_(std::move(symbol)),
      api_(api),
      strategy_(std::move(strategy)),
      pollInterval_(pollInterval),
      stopRequested_(stopRequested) {}

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
            logLine("[worker] symbol=" + symbol_ +
                    " stopped_after_error=\"" + exception.what() + '"');
            std::lock_guard<std::mutex> lock(stateMutex_);
            stateMachine_.transition(WorkerEvent::Failure);
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
        case WorkerState::PositionOpen:
            handlePosition();
            break;
        case WorkerState::EntryPending:
            // TODO: Poll broker order status and transition on final status.
            break;
        case WorkerState::ExitPending:
            // TODO: Poll broker exit order status and transition on final status.
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
    const StrategyDecision decision = strategy_->evaluateEntry(snapshot);
    const StrategyDiagnostics indicators = strategy_->diagnostics();
    logMarketStatus(
        snapshot,
        indicators,
        decision == StrategyDecision::Enter ? "ENTRY" : "NO_ENTRY",
        stateMachine_.state());

    if (decision == StrategyDecision::Enter) {
        stateMachine_.transition(WorkerEvent::EntrySignal);

        // TODO: Use a real entry fill price returned by the broker.
        const OrderResult order = api_.placeMarketOrder(
            symbol_, true, quantity_);

        if (order.accepted) {
            entryPrice_ = snapshot.lastPrice;
            stateMachine_.transition(WorkerEvent::EntryAccepted);
            logLine("[worker] symbol=" + symbol_ +
                    " order=accepted order_id=" + order.orderId);
        } else {
            stateMachine_.transition(WorkerEvent::EntryRejected);
            logLine("[worker] symbol=" + symbol_ +
                    " order=not_placed reason=\"" + order.message +
                    "\" state=" + toString(stateMachine_.state()));
        }
    }
}

void Worker::handlePosition() {
    const MarketSnapshot snapshot = api_.getQuote(symbol_);
    entryPrice_ = entryPrice_ <= 0.0 ? snapshot.lastPrice : entryPrice_;

    const StrategyDecision decision =
        strategy_->evaluateExit(snapshot, entryPrice_);
    const StrategyDiagnostics indicators = strategy_->diagnostics();
    logMarketStatus(
        snapshot,
        indicators,
        decision == StrategyDecision::Exit ? "EXIT" : "HOLD",
        stateMachine_.state());

    if (decision == StrategyDecision::Exit) {
        handleExit();
    }
}

void Worker::handleExit() {
    stateMachine_.transition(WorkerEvent::ExitSignal);
    const OrderResult order = api_.exitPosition(symbol_, quantity_);

    if (order.accepted) {
        stateMachine_.transition(WorkerEvent::ExitCompleted);
        logLine("[worker] symbol=" + symbol_ + " exit=accepted");
    } else {
        stateMachine_.transition(WorkerEvent::ExitRejected);
        logLine("[worker] symbol=" + symbol_ +
                " exit=not_placed reason=\"" + order.message + '"');
    }
}

void Worker::stop() {
    std::lock_guard<std::mutex> lock(stateMutex_);

    // On shutdown, attempt to close an open position before moving to Stopped.
    // In a real system this must wait for the broker's final order status.
    if (stateMachine_.state() == WorkerState::PositionOpen) {
        stateMachine_.transition(WorkerEvent::ExitSignal);
        const OrderResult order = api_.exitPosition(symbol_, quantity_);
        if (order.accepted) {
            stateMachine_.transition(WorkerEvent::ExitCompleted);
        } else {
            stateMachine_.transition(WorkerEvent::ExitRejected);
            logLine("[worker] symbol=" + symbol_ +
                    " shutdown_exit=not_placed reason=\"" +
                    order.message + '"');
        }
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
