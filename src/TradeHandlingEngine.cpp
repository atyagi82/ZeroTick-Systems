#include "zerotick/TradeHandlingEngine.h"
#include "zerotick/Log.h"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace zerotick {

namespace {

double roundUpToTick(double price, double tick) {
    return std::ceil(price / tick - 1e-9) * tick;
}

double priceTick(const MarketSnapshot& snapshot) {
    double smallestStep = 0.0;
    for (std::size_t level = 1; level < 5; ++level) {
        const double steps[] = {
            snapshot.bidLevels[level - 1].price - snapshot.bidLevels[level].price,
            snapshot.askLevels[level].price - snapshot.askLevels[level - 1].price
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

TradeHandlingEngine::TradeHandlingEngine(
    KiteApi& api, StockEngineRegistry& registry,
    ThreadSafeQueue<TradeSignal>& inputQueue,
    PortfolioRiskManager& portfolioRisk, const HandlingConfig& config)
    : api_(api), registry_(registry), inputQueue_(inputQueue),
      portfolioRisk_(portfolioRisk), config_(config) {}

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
                      (config_.liveTrading ? "enabled" : "disabled"));
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
    while (!stopRequested_.load() &&
           std::chrono::steady_clock::now() < deadline) {
        try {
            status = api_.getOrderStatus(orderId);
        } catch (const std::exception& exception) {
            zerotick::logLine("[handling] order=" + orderId +
                              " status_poll_failed=\"" + exception.what() +
                              "\"");
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
        outcome.exited = true;
        outcome.exitPrice = signal.targetPrice;
        outcome.stopLoss = false;
        outcome.reason = "paper_target";
        zerotick::logLine(
            "[handling] symbol=" + symbol + " paper_exit qty=" +
            std::to_string(quantity) + " price=" +
            std::to_string(outcome.exitPrice) + " reason=" + outcome.reason);
        portfolioRisk_.releaseSlot(symbol);
        registry_.release(symbol, EngineId::Handling);
        return;
    }

    OrderResult exitOrder;
    try {
        exitOrder = api_.placeLimitExit(symbol, quantity, signal.targetPrice);
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
            MarketSnapshot snapshot = api_.getQuote(symbol);
            if (snapshot.lastPrice > 0.0 &&
                snapshot.lastPrice <= signal.stopLossPrice && !stopLoss) {
                stopLoss = true;
                try {
                    api_.cancelOrder(activeExitOrderId);
                } catch (const std::exception&) {
                }
                try {
                    OrderResult stopExit = api_.placeLimitExit(
                        symbol,
                        remainingQuantity > 0 ? remainingQuantity : quantity,
                        signal.stopLossPrice);
                    activeExitOrderId = stopExit.orderId;
                    zerotick::logLine(
                        "[handling] symbol=" + symbol +
                        " stop_loss_hit exit_order=" + activeExitOrderId);
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

    double freeCash = config_.liveTrading
                          ? api_.cashAvailability().freeCash
                          : 100000.0;
    double slotBudget = 0.0;
    if (!portfolioRisk_.reserveSlot(symbol, freeCash, slotBudget)) {
        zerotick::logLine(
            "[handling] symbol=" + symbol +
            " rejected=portfolio_slot_unavailable");
        registry_.release(symbol, EngineId::Handling);
        return;
    }

    int quantity = zerotick::calculateQuantityWithinBudget(
        slotBudget, signal.entryLimitPrice > 0.0 ? signal.entryLimitPrice
                                                 : signal.entryTriggerPrice);
    if (quantity < 1) {
        zerotick::logLine(
            "[handling] symbol=" + symbol +
            " rejected=insufficient_budget_for_one_share");
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
                signal.entryLimitPrice);
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
                " entry_not_filled order=" + entryOrder.orderId);
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
    } else {
        entryFilled = true;
        entryPrice = signal.entryLimitPrice;
        filledQuantity = quantity;
        zerotick::logLine(
            "[handling] symbol=" + symbol + " paper_entry qty=" +
            std::to_string(filledQuantity) + " price=" +
            std::to_string(entryPrice));
    }

    manageExit(signal, filledQuantity, entryPrice);
}

} // namespace zerotick



