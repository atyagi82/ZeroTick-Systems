#include "zerotick/TradingDecisionEngine.h"
#include "zerotick/Log.h"
#include "zerotick/StructuredEventWriter.h"

#include <cmath>
#include <limits>
#include <sstream>

namespace zerotick {

namespace {

double roundUpToTick(double price, double tick) {
    return std::ceil(price / tick - 1e-9) * tick;
}

double roundDownToTick(double price, double tick) {
    return std::floor(price / tick + 1e-9) * tick;
}

const char* sideName(PositionSide side) {
    return side == PositionSide::Short ? "SHORT" : "LONG";
}

} // namespace

TradingDecisionEngine::TradingDecisionEngine(
    KiteApi& api, StockEngineRegistry& registry,
    ThreadSafeQueue<StockCandidate>& inputQueue,
    ThreadSafeQueue<TradeSignal>& outputQueue,
    const DecisionConfig& config, StructuredEventWriter* eventWriter)
    : api_(api), registry_(registry), inputQueue_(inputQueue),
      outputQueue_(outputQueue), config_(config), eventWriter_(eventWriter) {}

TradingDecisionEngine::~TradingDecisionEngine() { stop(); }

void TradingDecisionEngine::start() {
    if (!threads_.empty()) {
        throw std::logic_error("TradingDecisionEngine already started");
    }
    stopRequested_.store(false);
    threads_.reserve(config_.workerCount);
    for (std::size_t index = 0; index < config_.workerCount; ++index) {
        threads_.emplace_back([this] {
            this->workerLoop();
        });
    }
    zerotick::logLine("[decision] started " +
                      std::to_string(config_.workerCount) +
                      " worker threads");
}

void TradingDecisionEngine::stop() {
    stopRequested_.store(true);
    inputQueue_.done();
    for (std::thread& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
}

void TradingDecisionEngine::workerLoop() {
    while (!stopRequested_.load()) {
        StockCandidate candidate;
        if (!inputQueue_.waitPop(candidate)) {
            // Queue is done and empty.
            continue;
        }

        // Take ownership from the selection engine. If it has already moved
        // (should not happen), skip this candidate.
        if (!registry_.claim(candidate.symbol, EngineId::Selection,
                             EngineId::Decision)) {
            continue;
        }

        MarketSnapshot fresh;
        TradeSignal signal;
        std::string rejectionReason;
        bool confirmed = true;
        std::chrono::steady_clock::time_point previousReceipt;
        for (std::size_t confirmation = 0;
             confirmation < config_.requiredConfirmations;
             ++confirmation) {
            if (confirmation > 0) {
                const auto deadline = std::chrono::steady_clock::now() +
                    config_.pollInterval * 2;
                do {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(50));
                    try {
                        fresh = api_.getQuote(
                            candidate.symbol, config_.maxQuoteAge);
                    } catch (const std::exception&) {
                        continue;
                    }
                } while (!stopRequested_.load() &&
                         fresh.receivedAt <= previousReceipt &&
                         std::chrono::steady_clock::now() < deadline);
                if (fresh.receivedAt <= previousReceipt) {
                    rejectionReason = "confirmation_quote_not_updated";
                    confirmed = false;
                    break;
                }
            } else {
                try {
                    fresh = api_.getQuote(
                        candidate.symbol, config_.maxQuoteAge);
                } catch (const std::exception& exception) {
                    zerotick::logLine(
                        "[decision] symbol=" + candidate.symbol +
                        " quote_failed=\"" + exception.what() + "\"");
                    rejectionReason = "quote_failed";
                    confirmed = false;
                    break;
                }
            }
            previousReceipt = fresh.receivedAt;
            TradeSignal confirmationSignal;
            if (!evaluateDecision(
                    candidate, fresh, confirmationSignal,
                    rejectionReason)) {
                confirmed = false;
                break;
            }
            signal = confirmationSignal;
            if (confirmation + 1 < config_.requiredConfirmations) {
                std::this_thread::sleep_for(config_.pollInterval);
            }
        }

        if (confirmed) {
            signal.candidateId = candidate.candidateId;
            if (eventWriter_) {
                signal.signalId = eventWriter_->recordDecision(
                    candidate, fresh, true, signal, "passed");
            }
            // Keep Decision ownership until the handling engine claims the
            // signal off the output queue.
            outputQueue_.push(signal);
            zerotick::logLine(
                "[decision] symbol=" + signal.symbol + " decision=TRADE " +
                "side=" + sideName(signal.side) +
                " trigger=" + std::to_string(signal.entryTriggerPrice) +
                " limit=" + std::to_string(signal.entryLimitPrice) +
                " target=" + std::to_string(signal.targetPrice) +
                " stop=" + std::to_string(signal.stopLossPrice) +
                " qty=" + std::to_string(signal.quantity) +
                " confirmations=" +
                std::to_string(config_.requiredConfirmations));
        } else {
            if (eventWriter_) {
                eventWriter_->recordDecision(
                    candidate, fresh, false, signal, rejectionReason);
            }
            registry_.release(candidate.symbol, EngineId::Decision);
        }
    }
}

double TradingDecisionEngine::weightedImbalance(
    const MarketSnapshot& snapshot) const {
    // Nearer book levels matter more, so each deeper level gets less weight.
    double weightedBid = 0.0;
    double weightedAsk = 0.0;
    for (std::size_t level = 0; level < 5; ++level) {
        weightedBid += config_.imbalanceLevelWeights[level] *
                       static_cast<double>(snapshot.bidLevels[level].quantity);
        weightedAsk += config_.imbalanceLevelWeights[level] *
                       static_cast<double>(snapshot.askLevels[level].quantity);
    }
    const double total = weightedBid + weightedAsk;
    // Normalize to [-1, +1]; positive values indicate stronger bid depth.
    return total > 0.0 ? (weightedBid - weightedAsk) / total : 0.0;
}

double priceTick(const MarketSnapshot& snapshot, double fallbackTickSize) {
    // Infer a usable tick from adjacent depth prices, falling back when the
    // quote does not provide enough valid levels.
    double smallestStep = 0.0;
    for (std::size_t level = 1; level < 5; ++level) {
        const double previousBid = snapshot.bidLevels[level - 1].price;
        const double currentBid = snapshot.bidLevels[level].price;
        const double previousAsk = snapshot.askLevels[level - 1].price;
        const double currentAsk = snapshot.askLevels[level].price;
        const double steps[] = {
            previousBid > 0.0 && currentBid > 0.0
                ? previousBid - currentBid
                : 0.0,
            previousAsk > 0.0 && currentAsk > 0.0
                ? currentAsk - previousAsk
                : 0.0
        };
        for (double step : steps) {
            const double normalizedStep = std::round(step * 100.0) / 100.0;
            if (normalizedStep > 0.0 &&
                (smallestStep == 0.0 || normalizedStep < smallestStep)) {
                smallestStep = normalizedStep;
            }
        }
    }
    return smallestStep > 0.0 ? smallestStep : fallbackTickSize;
}

bool TradingDecisionEngine::evaluateDecision(
    const StockCandidate& candidate, const MarketSnapshot& fresh,
    TradeSignal& signal, std::string& rejectionReason) const {
    const std::string& symbol = candidate.symbol;
    if (candidate.side == PositionSide::Short &&
        !config_.allowShortSelling) {
        rejectionReason = "short_selling_disabled";
        return false;
    }

    // Liquidity re-check on the fresh snapshot.
    const double mid = (fresh.bidPrice + fresh.askPrice) / 2.0;
    const double spreadPercent =
        mid > 0.0 && fresh.askPrice >= fresh.bidPrice &&
                fresh.bidPrice > 0.0
            ? (fresh.askPrice - fresh.bidPrice) / mid * 100.0
            : 0.0;
    if (spreadPercent > config_.maximumSpreadPercent) {
        rejectionReason = "spread";
        return false;
    }

    // Volume confirmation carried from selection.
    if (config_.requireVolumeConfirmation &&
        candidate.relativeVolume < config_.minRelativeVolume) {
        rejectionReason = "relative_volume";
        return false;
    }

    // The order book must support the selected direction.
    const double imbalance = weightedImbalance(fresh);
    if ((candidate.side == PositionSide::Long &&
         imbalance < config_.minimumImbalance) ||
        (candidate.side == PositionSide::Short &&
         imbalance > -config_.minimumImbalance)) {
        rejectionReason = "order_book_imbalance";
        return false;
    }

    // Reject a move against the selected direction before entering.
    if (fresh.lastPrice > 0.0 && candidate.lastPrice > 0.0 &&
        ((candidate.side == PositionSide::Long &&
          fresh.lastPrice < candidate.lastPrice) ||
         (candidate.side == PositionSide::Short &&
          fresh.lastPrice > candidate.lastPrice))) {
        rejectionReason = "price_reversal";
        return false;
    }

    // Build the trade signal.
    signal.symbol = symbol;
    signal.side = candidate.side;
    signal.createdAt = std::chrono::steady_clock::now();

    const double tick = priceTick(fresh, config_.fallbackTickSize);
    const double triggerOffset =
        tick * static_cast<double>(config_.entryTriggerOffsetTicks);
    const double limitOffset =
        tick * static_cast<double>(config_.entryLimitOffsetTicks);
    double entryTrigger = 0.0;
    double entryLimit = 0.0;
    if (signal.side == PositionSide::Long) {
        const double triggerBase =
            fresh.lastPrice > fresh.askPrice ? fresh.lastPrice : fresh.askPrice;
        // Buy after an upward breakout, with the limit above the trigger.
        entryTrigger = roundUpToTick(triggerBase, tick) + triggerOffset;
        entryLimit = entryTrigger + limitOffset;
    } else {
        const double triggerBase =
            fresh.lastPrice < fresh.bidPrice ? fresh.lastPrice : fresh.bidPrice;
        // Sell after a downward breakdown, with the limit below the trigger.
        entryTrigger = roundDownToTick(triggerBase, tick) - triggerOffset;
        entryLimit = entryTrigger - limitOffset;
    }

    signal.entryTriggerPrice = entryTrigger;
    signal.entryLimitPrice = entryLimit;
    // Configured target/stop values are percentage points (0.50 means 0.50%).
    const double targetMultiplier =
        1.0 + config_.targetPercent / 100.0;
    const double shortTargetMultiplier =
        1.0 - config_.targetPercent / 100.0;
    const double stopMultiplier =
        1.0 - config_.stopPercent / 100.0;
    const double shortStopMultiplier =
        1.0 + config_.stopPercent / 100.0;
    signal.targetPrice = signal.side == PositionSide::Long
        ? entryLimit * targetMultiplier
        : entryLimit * shortTargetMultiplier;
    signal.stopLossPrice = signal.side == PositionSide::Long
        ? entryLimit * stopMultiplier
        : entryLimit * shortStopMultiplier;

    // Indicative quantity from a default entry budget; the handling engine
    // performs the final risk-checked sizing against the portfolio budget.
    const double perShare = entryLimit > 0.0 ? entryLimit : fresh.lastPrice;
    int quantity = 1;
    if (perShare > 0.0) {
        const double budgetQuantity =
            std::floor(config_.indicativeEntryBudget / perShare);
        quantity = budgetQuantity >=
                           static_cast<double>(
                               std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(budgetQuantity);
    }
    if (quantity < 1) {
        quantity = 1;
    }
    signal.quantity = quantity;

    return true;
}

} // namespace zerotick

