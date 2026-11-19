# Low-Level Design

**Project:** ZeroTick-Systems  
**Design snapshot:** 2026-11-19  
**Language standard:** C++14

## 1. Code map

| Area | Main files | Responsibility |
|---|---|---|
| Application lifecycle | `src/main.cpp` | Parse flags, authenticate, seed quotes, wire queues and engines, poll quotes, shut down |
| Broker abstraction | `include/zerotick/KiteApi.h` | Market-data and order interface, Kite adapter declaration |
| Kite implementation | `src/KiteConnectApi.cpp` | OAuth callback, HTTP requests, quote parsing/cache, cash, order APIs |
| Domain and concurrency | `include/zerotick/Types.h`, `include/zerotick/EngineTypes.h`, `src/EngineTypes.cpp` | Snapshots, candidates, signals, NIFTY 100 universe, registry, thread-safe queue |
| Selection | `include/zerotick/StockSelectionEngine.h`, `src/StockSelectionEngine.cpp` | Consume quote events and filter candidates |
| Decision | `include/zerotick/TradingDecisionEngine.h`, `src/TradingDecisionEngine.cpp` | Re-check a candidate against a fresh quote and produce trade signals |
| Execution and risk | `include/zerotick/TradeHandlingEngine.h`, `src/TradeHandlingEngine.cpp`, `include/zerotick/Risk.h` | Reserve capacity, size, execute/paper-trade, monitor exits |
| Legacy strategy path | `src/Worker.cpp`, `src/ThreadPool.cpp`, `src/Strategy.cpp`, `src/StateMachine.cpp` | Earlier per-symbol strategy/state-machine implementation; not created by current `main` |
| Tests | `tests/EngineTests.cpp`, `tests/StrategyTests.cpp` | Engine pipeline, strategy, risk sizing, and simulated order-flow tests |

## 2. Core data types

### `MarketSnapshot`

Defined in `Types.h`. A parsed quote carries:

- Symbol, last price, best bid/ask and up to five bid/ask levels.
- Aggregate displayed bid/ask quantities and cumulative volume.
- Open, high, and low prices.
- A local `steady_clock` receipt time used for staleness checks.

The REST adapter timestamps snapshots when parsed. The current type has a
`sequence` field, but the quote adapter does not populate it.

### `StockCandidate`

Produced by selection and queued to decision. It contains the selected price,
volume and relative-volume ratio, price change, spread, depth, intraday range,
NIFTY 100 membership, and selection time.

### `TradeSignal`

Produced by decision and queued to handling. It contains symbol, quantity,
entry trigger/limit, target, stop, and creation time.

### `EngineId` and `StockEngineRegistry`

`EngineId` values are `Unassigned`, `Selection`, `Decision`, and `Handling`.
`StockEngineRegistry::claim(symbol, expected, next)` performs an atomic
ownership transition under a mutex. A claim fails for an empty symbol, a
same-owner transition, or an unexpected current owner.

### `ThreadSafeQueue<T>`

The shared queue uses a mutex and condition variable. `waitPop` blocks until a
value arrives or `done()` signals producer completion. Queue instances are
used for quote snapshots, candidates, and trade signals. The implementation is
unbounded; if consumers fall behind the one-second quote producer, queued
snapshots can accumulate. Selection ignores snapshots older than five seconds,
but stale queue entries still have to be drained.

## 3. Startup, quote delivery, and shutdown

1. `main` accepts `--login` and `--enable-live-trading`; unknown flags fail
   startup.
2. `KiteConnectApi` reads API credentials from `.env`, with environment
   variables taking precedence. If forced or no access token exists, it uses a
   browser login and a local callback listener, exchanges the request token,
   and persists the access token locally.
3. `NiftyUniverse` provides the 100-symbol NSE snapshot dated 2026-11-19.
4. The initial quote refresh runs in chunks of 40 before selection starts.
   `KiteConnectApi::refreshQuotes` updates its cache and returns only quote
   snapshots present in the response. `main` enqueues those snapshots.
5. In live mode only, startup checks for existing MIS exposure/open orders and
   reads free cash.
6. `main` starts selection, decision, and handling workers, then starts the
   one-second quote poller. Each refresh result is published to the quote queue.
7. On SIGINT/SIGTERM, the main loop stops the poller first, then closes and
   joins the selection, decision, and handling workers. It logs remaining
   registry ownership.

Missing symbols do not produce queue events. The adapter logs a missing symbol
once until that symbol returns in a later response, at which point it logs
recovery.

## 4. Selection worker

`StockSelectionEngine` has a shared quote-update queue and a configurable
worker pool. Every worker blocks in `waitPop`; there is no periodic cache scan.
For each snapshot:

1. Ignore the event if its local receipt time is more than five seconds old.
2. Claim `Unassigned -> Selection`; skip the event if another stage owns the
   symbol.
3. Apply the first failing filter in this order:

| Filter | Default |
|---|---:|
| Price must be greater than `minPrice` | 100 |
| Symbol must be in the configured NIFTY 100 universe | Required |
| Cumulative volume must meet `minAbsoluteVolume` | 50,000 |
| Relative volume must meet `minRelativeVolume` | 1.5 |
| Spread must be within `maxSpreadPercent` | 0.20% |
| Both displayed depth sides must meet `minDepthQuantity` | 1,000 |
| Intraday range must meet the minimum | 0.30% |
| Intraday range and absolute price change must not exceed the maximum | 6.0% |

Relative volume uses an exponentially weighted average with `alpha = 0.2`.
The first observed volume initializes the average, so its relative volume is
1.0. Subsequent events update the EWMA from cumulative volume snapshots.

On rejection, selection releases the symbol. On success, it pushes a
`StockCandidate` and keeps ownership until decision claims it. Per-worker
summary logs are emitted at the configured five-second interval while quote
events are being processed; counts include selected symbols, stale events,
owned-symbol skips, and rejection reasons.

`SelectionConfig::minPriceChangePercent` is declared but is not currently
checked by `evaluateSelection`.

## 5. Decision worker

Each `TradingDecisionEngine` worker blocks on the candidate queue:

1. Claim `Selection -> Decision`.
2. Call `KiteApi::getQuote(candidate.symbol)`. The adapter rejects absent or
   cached snapshots older than five seconds; failures are logged and ownership
   is released.
3. Re-check the fresh spread against 0.10%.
4. If volume confirmation is enabled, require candidate relative volume of at
   least 1.2.
5. Compute weighted book imbalance using weights `1.0, 0.8, 0.6, 0.4, 0.2`
   for levels 1 through 5, and require at least `+0.05`.
6. Reject if the fresh last price is below the selected price.
7. Infer tick size from adjacent depth prices, rounded to paise; use `0.01`
   when no step can be inferred.
8. Calculate:

```text
triggerBase = max(fresh.lastPrice, fresh.askPrice)
trigger     = roundUpToTick(triggerBase, tick) + tick
limit       = trigger + tick
target      = limit * (1 + targetPercent / 100)
stop        = limit * (1 - stopPercent / 100)
```

The defaults are a 0.20% target and 0.40% stop. The signal quantity is an
indicative cap derived from a fixed 50,000 budget divided by entry limit; final
quantity is recalculated by handling.

On a rejected decision, release `Decision -> Unassigned`. On approval, enqueue
the signal and keep ownership until handling claims it.

## 6. Handling and portfolio risk

Each handling worker blocks on the signal queue and claims
`Decision -> Handling`.

### Slot and quantity calculation

`PortfolioRiskManager` limits concurrent reservations to ten by default and
prevents duplicate reservations for one symbol. Its slot budget is the lower
of current free cash and startup total cash divided by the maximum position
count. Quantity is `floor(slotBudget / entryLimitPrice)`, must be at least one,
and is capped by the decision signal quantity.

### Paper path

When `liveTrading` is false, handling uses a 100,000 free-cash value, marks the
entry at the signal limit, simulates a target exit, then releases the slot and
symbol. No Kite orders are submitted.

### Live path

When `liveTrading` is true:

1. Read current cash and reserve a slot.
2. Submit a Kite MIS stop-limit buy entry.
3. Poll status at `HandlingConfig::pollInterval` until terminal or the
   30-second entry timeout. On timeout, request cancellation.
4. If completed with a fill, monitor the target exit order and fetch quotes
   while waiting. If price reaches the stop, cancel the target order and
   submit a limit sell at the stop price.
5. Release slot and symbol when the attempt terminates.

Target/stop management is in-process; no broker-hosted OCO/bracket order is
created. The Kite adapter also exposes `estimateMisOrderFunds`, but the current
`TradeHandlingEngine` sizing path does not call it.

In the current `main`, `PortfolioRiskManager` is initialized from live startup
cash only when live mode is enabled. In paper mode that value remains zero,
which means the actual CLI paper path cannot reserve a slot. The paper-mode
tests construct the risk manager with a nonzero budget, so they do not cover
this CLI configuration gap.

## 7. Kite adapter details

`KiteConnectApi` implements the `KiteApi` interface:

| Operation | Kite API use |
|---|---|
| Quote refresh | Batched `/quote` requests; snapshots parsed from `last_price`, `depth`, `volume`, and `ohlc` |
| Fresh quote read | In-memory cache lookup with a five-second receipt-age limit |
| Live preflight | `/portfolio/positions` and `/orders`; block on existing MIS exposure for universe symbols |
| Cash | `/user/margins/equity`; free cash is validated and adjusted for utilization |
| MIS margin estimate | `/margins/orders` |
| Entry and exit | `/orders/regular` with MIS stop-limit buy and limit sell |
| Order status | `/orders/{order_id}` |
| Cancellation | Kite order-cancel endpoint |

HTTP, JSON, authentication, and broker errors are surfaced as exceptions.
Quote responses that omit an instrument are treated as unavailable instead of
causing a missing-key JSON exception.

## 8. Configuration and diagnostics

The engine defaults are defined in `SelectionConfig`, `DecisionConfig`, and
`HandlingConfig` in `EngineTypes.h`. Start logs report active selection
thresholds. Selection emits periodic aggregate rejections rather than one log
per quote. `Log.h` serializes writes so concurrent worker messages remain
intact.

Key flags:

- No flags: use a cached Kite access token when present; paper execution mode.
- `--login`: force browser login and token exchange.
- `--enable-live-trading`: enable actual MIS order submission; this is not a
  simulation flag.

## 9. Validation

Configure, build, and run tests:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`engine_tests` covers the 100-symbol universe, ownership transitions, event
waiting, selection filtering, decision signal generation, paper handling, and
an end-to-end paper-mode pipeline. `strategy_tests` covers the older
`OrderImbalanceStrategy`/`Worker` path and risk/order-flow cases.

## 10. Related documents

- [High-Level Design](./High%20Level%20Design.md)
