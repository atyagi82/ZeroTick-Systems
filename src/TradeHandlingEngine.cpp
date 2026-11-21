#include "zerotick/TradeHandlingEngine.h"
#include "zerotick/Log.h"
#include "zerotick/StructuredEventWriter.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace zerotick {

namespace {

double roundUpToTick(double price, double tick) {
    return std::ceil(price / tick - 1e-9) * tick;
}

const char* sideName(PositionSide side) {
    return side == PositionSide::Short ? "SHORT" : "LONG";
}

} // namespace

TradeHandlingEngine::TradeHandlingEngine(
    KiteApi& api, StockEngineRegistry& registry,
    ThreadSafeQueue<TradeSignal>& inputQueue,
    PortfolioRiskManager& portfolioRisk, const HandlingConfig& config,
    StructuredEventWriter* eventWriter)
    : api_(api), registry_(registry), inputQueue_(inputQueue),
      portfolioRisk_(portfolioRisk), config_(config),
      eventWriter_(eventWriter) {}

TradeHandlingEngine::~TradeHandlingEngine() { stop(); }

void TradeHandlingEngine::start() {
    if (!threads_.empty()) {
        throw std::logic_error("TradeHandlingEngine already started");
    }
    stopRequested_.store(false);
    threads_.reserve(config_.workerCount);
    for (std::size_t index = 0; index < config_.workerCount; ++index) {
        threads_.emplace_back([this] {
            this->workerLoop();
        });
    }
    zerotick::logLine("[handling] started " +
                      std::to_string(config_.workerCount) +
                      " worker threads; live_trading=" +
                      (config_.liveTrading ? "enabled" : "disabled") +
                      " shadow_live=" +
                      (config_.shadowLiveSimulation ? "enabled" : "disabled"));
}

bool TradeHandlingEngine::waitForShadowEntry(
    const TradeSignal& signal, int quantity, double& entryPrice) {
    if (eventWriter_) {
        eventWriter_->recordTradeEvent(
            signal, "entry_pending", quantity, signal.entryLimitPrice,
            "waiting_for_live_quote");
    }
    const auto deadline =
        std::chrono::steady_clock::now() + config_.entryFillTimeout;
    while (!stopRequested_.load() &&
           std::chrono::steady_clock::now() < deadline) {
        try {
            const MarketSnapshot snapshot =
                api_.getQuote(signal.symbol, config_.maxQuoteAge);
            const bool triggerReached = signal.side == PositionSide::Long
                ? snapshot.lastPrice >= signal.entryTriggerPrice
                : snapshot.lastPrice <= signal.entryTriggerPrice;
            const double executablePrice = signal.side == PositionSide::Long
                ? snapshot.askPrice : snapshot.bidPrice;
            const bool withinLimit = executablePrice > 0.0 &&
                (signal.side == PositionSide::Long
                    ? executablePrice <= signal.entryLimitPrice
                    : executablePrice >= signal.entryLimitPrice);
            if (triggerReached && withinLimit) {
                entryPrice = executablePrice;
                if (eventWriter_) {
                    eventWriter_->recordTradeEvent(
                        signal, "entry_filled", quantity, entryPrice,
                        "live_quote_crossed_trigger", &snapshot);
                }
                return true;
            }
        } catch (const std::exception&) {
        }
        std::this_thread::sleep_for(config_.pollInterval);
    }
    if (eventWriter_) {
        eventWriter_->recordTradeEvent(
            signal, "entry_expired", quantity, 0.0,
            stopRequested_.load() ? "shutdown" : "entry_timeout");
    }
    return false;
}

void TradeHandlingEngine::manageShadowExit(
    const TradeSignal& signal, int quantity, double entryPrice) {
    while (!stopRequested_.load()) {
        try {
            const MarketSnapshot snapshot =
                api_.getQuote(signal.symbol, config_.maxQuoteAge);
            const bool stopTouched = signal.side == PositionSide::Long
                ? snapshot.lastPrice <= signal.stopLossPrice
                : snapshot.lastPrice >= signal.stopLossPrice;
            const bool targetTouched = signal.side == PositionSide::Long
                ? snapshot.lastPrice >= signal.targetPrice
                : snapshot.lastPrice <= signal.targetPrice;
            if (stopTouched || targetTouched) {
                double exitPrice = signal.side == PositionSide::Long
                    ? snapshot.bidPrice : snapshot.askPrice;
                if (exitPrice <= 0.0) exitPrice = snapshot.lastPrice;
                const std::string reason = stopTouched ? "stop_loss" : "target";
                if (eventWriter_) {
                    eventWriter_->recordTradeEvent(
                        signal, "position_closed", quantity, exitPrice,
                        reason, &snapshot);
                }
                zerotick::logLine(
                    "[shadow] symbol=" + signal.symbol + " side=" +
                    sideName(signal.side) + " qty=" +
                    std::to_string(quantity) + " entry=" +
                    std::to_string(entryPrice) + " exit=" +
                    std::to_string(exitPrice) + " reason=" + reason);
                portfolioRisk_.releaseSlot(signal.symbol);
                registry_.release(signal.symbol, EngineId::Handling);
                return;
            }
        } catch (const std::exception&) {
        }
        std::this_thread::sleep_for(config_.pollInterval);
    }
    if (eventWriter_) {
        eventWriter_->recordTradeEvent(
            signal, "position_interrupted", quantity, entryPrice, "shutdown");
    }
    portfolioRisk_.releaseSlot(signal.symbol);
    registry_.release(signal.symbol, EngineId::Handling);
}

void TradeHandlingEngine::stop() {
    stopRequested_.store(true);
    inputQueue_.done();
    for (std::thread& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
}

void TradeHandlingEngine::workerLoop() {
    while (!stopRequested_.load()) {
        TradeSignal signal;
        if (!inputQueue_.waitPop(signal)) {
            continue; // queue done and empty
        }

        // Take ownership from the decision engine.
        if (!registry_.claim(signal.symbol, EngineId::Decision,
                             EngineId::Handling)) {
            continue;
        }

        handleSignal(signal);
    }
}

bool TradeHandlingEngine::waitForEntryFill(const std::string& orderId,
                                           BrokerOrderStatus& status) const {
    const auto deadline =
        std::chrono::steady_clock::now() + config_.entryFillTimeout;
    // Transient status failures do not imply an order was rejected; keep
    // polling until it becomes terminal or the configured deadline expires.
    while (!stopRequested_.load() &&
           std::chrono::steady_clock::now() < deadline) {
        try {
            status = api_.getOrderStatus(orderId);
        } catch (const std::exception& exception) {
            zerotick::logLine(
                std::string("[handling] entry_status_poll_failed=\"") +
                exception.what() + "\"");
            std::this_thread::sleep_for(config_.pollInterval);
            continue;
        }
        if (status.terminal) {
            return true;
        }
        std::this_thread::sleep_for(config_.pollInterval);
    }
    return false;
}

bool TradeHandlingEngine::waitForExitFill(const std::string& orderId,
                                          BrokerOrderStatus& status) const {
    const auto deadline =
        std::chrono::steady_clock::now() + config_.exitFillTimeout;
    while (!stopRequested_.load() &&
           std::chrono::steady_clock::now() < deadline) {
        try {
            status = api_.getOrderStatus(orderId);
        } catch (const std::exception&) {
            std::this_thread::sleep_for(config_.pollInterval);
            continue;
        }
        if (status.terminal) {
            return true;
        }
        std::this_thread::sleep_for(config_.pollInterval);
    }
    return false;
}

void TradeHandlingEngine::manageExit(const TradeSignal& signal, int quantity,
                                     double entryPrice) {
    const std::string& symbol = signal.symbol;
    TradeOutcome outcome;
    outcome.symbol = symbol;
    outcome.entryFilled = true;
    outcome.filledEntryPrice = entryPrice;
    outcome.filledQuantity = quantity;

    if (!config_.liveTrading) {
        // Paper mode is a deterministic pipeline check, not a market replay:
        // it records an entry and immediately models an exit at the target.
        outcome.exited = true;
        outcome.exitPrice = signal.targetPrice;
        outcome.stopLoss = false;
        outcome.reason = "paper_target";
        zerotick::logLine(
            "[handling] symbol=" + symbol + " side=" +
            sideName(signal.side) + " paper_exit qty=" +
            std::to_string(quantity) + " price=" +
            std::to_string(outcome.exitPrice) + " reason=" + outcome.reason);
        portfolioRisk_.releaseSlot(symbol);
        registry_.release(symbol, EngineId::Handling);
        return;
    }

    OrderResult exitOrder;
    try {
        // The position side determines the closing transaction: long exits
        // sell, while short exits buy to cover.
        exitOrder = api_.placeLimitExit(
            symbol, quantity, signal.targetPrice, signal.side);
    } catch (const std::exception& exception) {
        zerotick::logLine(
            "[handling] symbol=" + symbol +
            " exit_submit_failed=\"" + exception.what() + "\"");
        portfolioRisk_.releaseSlot(symbol);
        registry_.release(symbol, EngineId::Handling);
        return;
    }

    BrokerOrderStatus status;
    bool exited = false;
    bool stopLoss = false;
    double exitPrice = 0.0;
    std::string reason = "timeout";
    std::string activeExitOrderId = exitOrder.orderId;
    int remainingQuantity = quantity;

    while (!stopRequested_.load()) {
        try {
            status = api_.getOrderStatus(activeExitOrderId);
        } catch (const std::exception&) {
            std::this_thread::sleep_for(config_.pollInterval);
            continue;
        }

        if (status.terminal) {
            exited = true;
            exitPrice = status.averagePrice;
            remainingQuantity -= status.filledQuantity;
            reason = (status.status == "REJECTED" ||
                      status.status == "CANCELLED")
                         ? "rejected"
                         : (stopLoss ? "stop_loss" : "target");
            break;
        }

        try {
            MarketSnapshot snapshot =
                api_.getQuote(symbol, config_.maxQuoteAge);
            const bool stopTouched =
                signal.side == PositionSide::Long
                    ? snapshot.lastPrice <= signal.stopLossPrice
                    : snapshot.lastPrice >= signal.stopLossPrice;
            if (snapshot.lastPrice > 0.0 && stopTouched && !stopLoss) {
                stopLoss = true;
                try {
                    // Replace the target order with a closing order at the
                    // stop level, using the side-aware exit transaction.
                    api_.cancelOrder(activeExitOrderId);
                } catch (const std::exception&) {
                }
                try {
                    OrderResult stopExit = api_.placeLimitExit(
                        symbol,
                        remainingQuantity > 0 ? remainingQuantity : quantity,
                        signal.stopLossPrice, signal.side);
                    activeExitOrderId = stopExit.orderId;
                    zerotick::logLine(
                        "[handling] symbol=" + symbol +
                        " stop_loss_hit replacement_order_submitted");
                } catch (const std::exception& exception) {
                    zerotick::logLine(
                        "[handling] symbol=" + symbol +
                        " stop_exit_failed=\"" + exception.what() + "\"");
                    reason = "stop_exit_failed";
                    break;
                }
            }
        } catch (const std::exception&) {
        }

        std::this_thread::sleep_for(config_.pollInterval);
    }

    outcome.exited = exited;
    outcome.exitPrice = exitPrice;
    outcome.stopLoss = stopLoss;
    outcome.reason = reason;

    zerotick::logLine(
        "[handling] symbol=" + symbol + " entry_filled=" +
        (outcome.entryFilled ? "true" : "false") +
        " side=" + sideName(signal.side) +
        " filled_qty=" + std::to_string(outcome.filledQuantity) +
        " entry_price=" + std::to_string(outcome.filledEntryPrice) +
        " exited=" + (outcome.exited ? "true" : "false") +
        " exit_price=" + std::to_string(outcome.exitPrice) +
        " stop_loss=" + (outcome.stopLoss ? "true" : "false") +
        " reason=" + outcome.reason);

    portfolioRisk_.releaseSlot(symbol);
    registry_.release(symbol, EngineId::Handling);
}

void TradeHandlingEngine::handleSignal(const TradeSignal& signal) {
    const std::string& symbol = signal.symbol;

    if (signal.side == PositionSide::Short &&
        !config_.allowShortSelling) {
        zerotick::logLine(
            "[handling] symbol=" + symbol +
            " rejected=short_selling_disabled");
        if (eventWriter_) eventWriter_->recordTradeEvent(
            signal, "entry_rejected", 0, 0.0, "short_selling_disabled");
        registry_.release(symbol, EngineId::Handling);
        return;
    }

    double freeCash = config_.liveTrading
                          ? api_.cashAvailability().freeCash
                          : config_.paperTradingCash;
    double slotBudget = 0.0;
    if (!portfolioRisk_.reserveSlot(symbol, freeCash, slotBudget)) {
        zerotick::logLine(
            "[handling] symbol=" + symbol +
            " rejected=portfolio_slot_unavailable");
        if (eventWriter_) eventWriter_->recordTradeEvent(
            signal, "entry_rejected", 0, 0.0, "portfolio_slot_unavailable");
        registry_.release(symbol, EngineId::Handling);
        return;
    }

    // Reserve capacity before sizing so concurrent handlers cannot overbook
    // the same slot; never exceed the signal's indicative quantity either.
    int quantity = zerotick::calculateQuantityWithinBudget(
        slotBudget, signal.entryLimitPrice > 0.0 ? signal.entryLimitPrice
                                                 : signal.entryTriggerPrice);
    if (quantity < 1) {
        zerotick::logLine(
            "[handling] symbol=" + symbol +
            " rejected=insufficient_budget_for_one_share");
        if (eventWriter_) eventWriter_->recordTradeEvent(
            signal, "entry_rejected", 0, 0.0,
            "insufficient_budget_for_one_share");
        portfolioRisk_.releaseSlot(symbol);
        registry_.release(symbol, EngineId::Handling);
        return;
    }
    if (signal.quantity > 0 && quantity > signal.quantity) {
        quantity = signal.quantity;
    }

    bool entryFilled = false;
    double entryPrice = 0.0;
    int filledQuantity = 0;

    if (config_.liveTrading) {
        OrderResult entryOrder;
        try {
            entryOrder = api_.placeStopLimitEntry(
                symbol, quantity, signal.entryTriggerPrice,
                signal.entryLimitPrice, signal.side);
        } catch (const std::exception& exception) {
            zerotick::logLine(
                "[handling] symbol=" + symbol +
                " entry_submit_failed=\"" + exception.what() + "\"");
            portfolioRisk_.releaseSlot(symbol);
            registry_.release(symbol, EngineId::Handling);
            return;
        }

        BrokerOrderStatus status;
        if (!waitForEntryFill(entryOrder.orderId, status)) {
            zerotick::logLine(
                "[handling] symbol=" + symbol +
                " entry_not_filled");
            try {
                api_.cancelOrder(entryOrder.orderId);
            } catch (const std::exception&) {
            }
            portfolioRisk_.releaseSlot(symbol);
            registry_.release(symbol, EngineId::Handling);
            return;
        }
        if (status.status != "COMPLETE" || status.filledQuantity < 1) {
            zerotick::logLine(
                "[handling] symbol=" + symbol +
                " entry_rejected status=" + status.status);
            portfolioRisk_.releaseSlot(symbol);
            registry_.release(symbol, EngineId::Handling);
            return;
        }
        entryFilled = true;
        entryPrice = status.averagePrice;
        filledQuantity = status.filledQuantity;
    } else if (config_.shadowLiveSimulation) {
        if (!waitForShadowEntry(signal, quantity, entryPrice)) {
            portfolioRisk_.releaseSlot(symbol);
            registry_.release(symbol, EngineId::Handling);
            return;
        }
        entryFilled = true;
        filledQuantity = quantity;
    } else {
        entryFilled = true;
        entryPrice = signal.entryLimitPrice;
        filledQuantity = quantity;
        zerotick::logLine(
            "[handling] symbol=" + symbol + " side=" +
            sideName(signal.side) + " paper_entry qty=" +
            std::to_string(filledQuantity) + " price=" +
            std::to_string(entryPrice));
    }

    if (config_.shadowLiveSimulation) {
        manageShadowExit(signal, filledQuantity, entryPrice);
    } else {
        manageExit(signal, filledQuantity, entryPrice);
    }
}

} // namespace zerotick



