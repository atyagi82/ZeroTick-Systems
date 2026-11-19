#include "zerotick/TradingDecisionEngine.h"
#include "zerotick/Log.h"

#include <cmath>
#include <sstream>

namespace zerotick {

namespace {

double roundUpToTick(double price, double tick) {
    return std::ceil(price / tick - 1e-9) * tick;
}

} // namespace

TradingDecisionEngine::TradingDecisionEngine(
    KiteApi& api, StockEngineRegistry& registry,
    ThreadSafeQueue<StockCandidate>& inputQueue,
    ThreadSafeQueue<TradeSignal>& outputQueue,
    const DecisionConfig& config)
    : api_(api), registry_(registry), inputQueue_(inputQueue),
      outputQueue_(outputQueue), config_(config) {}

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
        try {
            fresh = api_.getQuote(candidate.symbol);
        } catch (const std::exception& exception) {
            zerotick::logLine(
                "[decision] symbol=" + candidate.symbol +
                " quote_failed=\"" + exception.what() + "\"");
            registry_.release(candidate.symbol, EngineId::Decision);
            continue;
        }

        TradeSignal signal;
        if (evaluateDecision(candidate, fresh, signal)) {
            // Keep Decision ownership until the handling engine claims the
            // signal off the output queue.
            outputQueue_.push(signal);
            zerotick::logLine(
                "[decision] symbol=" + signal.symbol + " decision=TRADE " +
                "trigger=" + std::to_string(signal.entryTriggerPrice) +
                " limit=" + std::to_string(signal.entryLimitPrice) +
                " target=" + std::to_string(signal.targetPrice) +
                " stop=" + std::to_string(signal.stopLossPrice) +
                " qty=" + std::to_string(signal.quantity));
        } else {
            registry_.release(candidate.symbol, EngineId::Decision);
        }
    }
}

double TradingDecisionEngine::weightedImbalance(
    const MarketSnapshot& snapshot) const {
    // Nearer book levels matter more, so each deeper level gets less weight.
    static constexpr double levelWeights[5] = {
        1.0, 0.8, 0.6, 0.4, 0.2
    };
    double weightedBid = 0.0;
    double weightedAsk = 0.0;
    for (std::size_t level = 0; level < 5; ++level) {
        weightedBid += levelWeights[level] *
                       static_cast<double>(snapshot.bidLevels[level].quantity);
        weightedAsk += levelWeights[level] *
                       static_cast<double>(snapshot.askLevels[level].quantity);
    }
    const double total = weightedBid + weightedAsk;
    // Normalize to [-1, +1]; positive values indicate stronger bid depth.
    return total > 0.0 ? (weightedBid - weightedAsk) / total : 0.0;
}

double priceTick(const MarketSnapshot& snapshot) {
    // Infer a usable tick from adjacent depth prices, falling back when the
    // quote does not provide enough valid levels.
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

bool TradingDecisionEngine::evaluateDecision(
    const StockCandidate& candidate, const MarketSnapshot& fresh,
    TradeSignal& signal) const {
    const std::string& symbol = candidate.symbol;

    // Liquidity re-check on the fresh snapshot.
    const double mid = (fresh.bidPrice + fresh.askPrice) / 2.0;
    const double spreadPercent =
        mid > 0.0 && fresh.askPrice >= fresh.bidPrice &&
                fresh.bidPrice > 0.0
            ? (fresh.askPrice - fresh.bidPrice) / mid * 100.0
            : 0.0;
    if (spreadPercent > config_.maximumSpreadPercent) {
        return false;
    }

    // Volume confirmation carried from selection.
    if (config_.requireVolumeConfirmation &&
        candidate.relativeVolume < config_.minRelativeVolume) {
        return false;
    }

    // Order-book imbalance must favor the bid side (buying pressure).
    const double imbalance = weightedImbalance(fresh);
    if (imbalance < config_.minimumImbalance) {
        return false;
    }

    // Price should not be falling from the selected level.
    if (fresh.lastPrice > 0.0 && candidate.lastPrice > 0.0 &&
        fresh.lastPrice < candidate.lastPrice) {
        return false;
    }

    // Build the trade signal.
    signal.symbol = symbol;
    signal.createdAt = std::chrono::steady_clock::now();

    const double tick = priceTick(fresh);
    const double triggerBase =
        fresh.lastPrice > fresh.askPrice ? fresh.lastPrice : fresh.askPrice;
    // Trigger above the greater of last/ask, then place the limit one tick
    // above the trigger to reduce the chance of an invalid stop-limit pair.
    const double entryTrigger =
        roundUpToTick(triggerBase, tick) + tick;
    const double entryLimit = entryTrigger + tick;

    signal.entryTriggerPrice = entryTrigger;
    signal.entryLimitPrice = entryLimit;
    // Configured target/stop values are percentage points (0.20 means 0.20%).
    signal.targetPrice = entryLimit * (1.0 + config_.targetPercent / 100.0);
    signal.stopLossPrice = entryLimit * (1.0 - config_.stopPercent / 100.0);

    // Indicative quantity from a default entry budget; the handling engine
    // performs the final risk-checked sizing against the portfolio budget.
    const double perShare = entryLimit > 0.0 ? entryLimit : fresh.lastPrice;
    int quantity = 1;
    if (perShare > 0.0) {
        quantity = static_cast<int>(std::floor(50000.0 / perShare));
    }
    if (quantity < 1) {
        quantity = 1;
    }
    signal.quantity = quantity;

    return true;
}

} // namespace zerotick

