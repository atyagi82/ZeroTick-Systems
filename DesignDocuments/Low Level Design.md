# Low-Level Design

**Project:** ZeroTick-Systems  
**Design snapshot:** 2026-11-22
**Language standard:** C++14

## 1. Code map

| Area | Main files | Responsibility |
|---|---|---|
| Application lifecycle | `src/main.cpp` | Load run mode, authenticate, wire engines, refresh stock quotes, and shut down |
| Historical backtest | `include/zerotick/BacktestEngine.h`, `src/BacktestEngine.cpp` | Expand OHLCV into deterministic one-second events, model execution/costs, and report net results |
| Broker abstraction | `include/zerotick/KiteApi.h` | Market-data and order interface, Kite adapter declaration |
| Kite implementation | `src/KiteConnectApi.cpp` | OAuth callback, HTTP requests, quote cache, instrument master, historical candles, cash, order APIs |
| Domain and concurrency | `include/zerotick/Types.h`, `include/zerotick/EngineTypes.h`, `src/EngineTypes.cpp` | Snapshots, candidates, signals, NIFTY 100 universe, registry, thread-safe queue |
| Selection | `include/zerotick/StockSelectionEngine.h`, `src/StockSelectionEngine.cpp` | Consume quote events and filter candidates |
| Decision | `include/zerotick/TradingDecisionEngine.h`, `src/TradingDecisionEngine.cpp` | Confirm candidates against fresh quotes and produce trade signals |
| Structured events | `include/zerotick/StructuredEventWriter.h`, `src/StructuredEventWriter.cpp` | Append shadow-live snapshots, decisions, execution events, and outcomes as JSONL |
| Execution and risk | `include/zerotick/TradeHandlingEngine.h`, `src/TradeHandlingEngine.cpp`, `include/zerotick/Risk.h` | Reserve capacity, size, execute/paper-trade, monitor exits |
| Tests | `tests/EngineTests.cpp` | Engine pipeline, selection thresholds, risk sizing, and simulated order-flow tests |

## 2. Core data types

### `MarketSnapshot`

Defined in `Types.h`. A parsed quote carries:

- Symbol, last price, best bid/ask and up to five bid/ask levels.
- Aggregate displayed bid/ask quantities, cumulative volume, and broker
  `average_price` (used as live VWAP when available).
- Open, high, and low prices.
- A local `steady_clock` receipt time used for staleness checks.

The REST adapter timestamps snapshots when parsed. The current type has a
`sequence` field, but the quote adapter does not populate it.

### `HistoricalCandle`

Defined in `Types.h`. It stores the Kite timestamp, OHLC prices, and volume
for one minute. Candle volume is per-bar, so the backtest sums it into
cumulative session volume before applying the live selection's volume rules.

### `StockCandidate`

Produced by selection and queued to decision. It contains the selected price,
volume and relative-volume ratio, 30-second momentum, 120-second trend change,
spread, depth, intraday range,
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

1. `main` accepts only optional `--config <path>`; unknown flags fail startup.
   Run mode, authentication behavior, short-selling opt-in, live confirmation,
   and backtest date/capital are validated from JSON before authentication.
2. `KiteConnectApi` reads API credentials from `.env`, with environment
   variables taking precedence. If forced or no access token exists, it uses a
   browser login and a local callback listener, exchanges the request token,
   and persists the access token locally.
3. `NiftyUniverse` provides the 100-symbol NSE snapshot dated 2026-11-19.
4. The initial quote refresh runs in chunks of 40 before selection starts.
   `KiteConnectApi::refreshQuotes` updates its cache and returns only quote
   snapshots present in the response.
5. In live mode only, startup checks for existing MIS exposure/open orders and
   reads free cash.
6. `main` starts selection, decision, and handling workers, then starts the
   one-second quote poller. Each returned stock result is published to
   selection.
7. On SIGINT/SIGTERM, the main loop stops the poller first, then closes and
   joins the selection, decision, and handling workers. It logs remaining
   registry ownership.

Missing symbols do not produce queue events. The adapter logs a missing symbol
once until that symbol returns in a later response, at which point it logs
recovery.

When `run.mode` is `backtest`, `main` branches before starting the live quote
poller or trading engines. It loads the NSE equity instrument dump to resolve
tokens and tick sizes, then requests one-minute candles for the NIFTY 100 from
09:15 to 15:30. Calls are spaced 400 ms apart.
Closed trades are printed in a text table, and the final summary line reports
total profit or loss. Live trading is rejected in backtest mode.

`config.json` is the operator-editable source for selection thresholds and
volume smoothing, decision thresholds and book-imbalance weights, quote
freshness, entry tick offsets, target/stop settings, worker counts and polling
intervals, fill timeouts, position and paper-cash limits, short-selling
opt-in, and backtest capital, tick-size fallback, and entry-time window.
Missing keys retain compiled defaults; unknown keys, malformed values, and
invalid ranges fail startup. Changes are read once when the executable starts,
not hot-reloaded. The file path is relative to the process working directory
unless an explicit `--config` path is provided.

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
| Relative volume must meet `minRelativeVolume` | 1.2 |
| Spread must be within `maxSpreadPercent` | 0.03% |
| Both displayed depth sides must meet `minDepthQuantity` | 1,000 |
| 30-second momentum must meet `minPriceChangePercent` | 0.12% |
| 120-second trend must align by `minTrendChangePercent` | 0.06% |
| Intraday range must meet the minimum | 0.30% |
| Intraday range and absolute price change must not exceed the maximum | 6.0% |

Relative volume uses volume added since the previous cumulative quote and an
exponentially weighted average with `alpha = 0.2`. The first cumulative sample
establishes the baseline with relative volume 1.0 and is not inserted as an
interval, so it cannot dominate later samples.

Price movement uses a bounded deque of timestamped observations. The quote
cannot qualify until both configured lookbacks have references. History
advances even when another selection filter rejects the quote; out-of-order
events cannot move it backward.

On rejection, selection releases the symbol. On success, it pushes a
`StockCandidate` and keeps ownership until decision claims it. Per-worker
summary logs are emitted at the configured five-second interval while quote
events are being processed; counts include selected symbols, stale events,
owned-symbol skips, and rejection reasons.

Short candidates require `allowShortSelling`; otherwise downward momentum is
rejected. The same setting is required independently by decision and handling,
so a short signal cannot reach an order submission path unless all stages were
explicitly configured. Runtime config sets all three guards from the single
top-level `allow_short_selling` value.

## 5. Historical candle replay

`BacktestEngine` accepts minute candles grouped by symbol. With
`synthesize_one_second_quotes` enabled, every
minute is deterministically expanded into second-level observations while
preserving the source OHLC path and total volume. Events from all symbols are
merged and replayed in timestamp order. Each symbol maintains cumulative
volume, session high/low, relative-volume state, and the same bounded 30- and
120-second price histories used by live selection.

Because candles do not contain an order book, the replay estimates spread as:

```text
min(maximumSpread,
    baseSpread + minuteRangePercent * volatilitySpreadFactor)
```

The estimate is applied to selection and decision spread gates and to
executable buy/sell prices. Displayed-depth and weighted-book-imbalance gates
cannot be reconstructed from candles and are therefore not asserted by the
replay.

A signal creates a pending stop-limit entry. It remains eligible for
`entry_timeout_seconds` (60 seconds in the checked-in config), with the legacy
minute value used only when the seconds value is absent. The configured entry
window is `[10:00, 14:45)` NSE time. Target, stop-loss, momentum-reversal, and
maximum-holding exits are evaluated after entry; the checked-in backtest hold
limit is 820 seconds. Any remaining position is closed at its final available
price.

The replay reserves at most `backtest.max_open_positions` concurrent slots.
Each position is capped by both available cash and
`initial_capital / max_open_positions`; this is not a daily trade-count limit.
Each completed round trip estimates capped brokerage, STT, exchange and SEBI
fees, stamp duty, and GST. Reports therefore show gross P&L, estimated charges,
ending capital, drawdown, and net P&L after costs. Spread and statutory costs
are approximations; order-book depth, imbalance, and extra market-impact
slippage remain outside the model.

The multi-day optimizer uses the same second-level replay, entry window,
spread model, and charge model for every parameter variant. Each
session resets intraday state while capital compounds between sessions. Current
NIFTY 100 membership is used for historical dates, so constituent membership
is not point-in-time accurate and results are not a guarantee of profitability.

## 6. Decision worker

Each `TradingDecisionEngine` worker blocks on the candidate queue:

1. Claim `Selection -> Decision`.
2. Obtain three fresh quotes, one second apart in the checked-in config. Each
   quote must be present, no older than five seconds, and directionally valid.
3. On every confirmation, require spread no wider than `0.03%`, candidate
   relative volume of at least 1.5 when enabled, and weighted depth imbalance
   using weights `1.0, 0.8, 0.6, 0.4, 0.2`. Longs require at least `+0.15`;
   shorts require at most `-0.15` and the short-selling opt-in.
4. Infer tick size from adjacent depth prices, falling back to the configured
   market-data tick size when no step can be inferred.
5. Construct the stop-limit entry and target/stop prices from configurable
   tick offsets and percentages:

```text
long:  triggerBase = max(fresh.lastPrice, fresh.askPrice)
       trigger     = roundUpToTick(triggerBase, tick) + triggerOffsetTicks * tick
       limit       = trigger + limitOffsetTicks * tick
       target/stop = limit * (1 +/- percentage / 100)

short: triggerBase = min(fresh.lastPrice, fresh.bidPrice)
       trigger     = roundDownToTick(triggerBase, tick) - triggerOffsetTicks * tick
       limit       = trigger - limitOffsetTicks * tick
       target/stop = limit * (1 -/+ percentage / 100)
```

The checked-in values are zero trigger-offset ticks, three limit-offset ticks,
a 0.50% target, and a 0.25% stop. The signal quantity is an indicative cap
derived from the configured 50,000 budget divided by entry limit; final
quantity is recalculated by handling.

On a rejected decision, release `Decision -> Unassigned`. On approval, enqueue
the signal and keep ownership until handling claims it.

## 7. Handling and portfolio risk

Each handling worker blocks on the signal queue and claims
`Decision -> Handling`.

### Slot and quantity calculation

`PortfolioRiskManager` limits concurrent reservations to the configured cap and
prevents duplicate reservations for one symbol. Its slot budget is the lower
of current free cash and startup total cash divided by the maximum position
count. Quantity is `floor(slotBudget / entryLimitPrice)`, must be at least one,
and is capped by the decision signal quantity.

### Paper path

Plain `paper` mode is a deterministic pipeline check: handling marks the entry
at the signal limit, simulates a target exit, and releases the slot and symbol.
`shadow_live` instead waits for a market-observed entry, monitors fresh quotes,
and exits on target, stop, confirmed imbalance reversal, or maximum holding
time. Neither mode submits Kite orders. Both use `risk.paper_trading_cash` and
the configured position cap. For long positions the exit is a sell; for short
positions it is a buy to cover. The `allowShortSelling` handling guard is a
final safety check.

### Live path

When `liveTrading` is true:

1. Read current cash and reserve a slot.
2. Submit a Kite MIS stop-limit entry in the side required by the signal.
3. Poll status at `HandlingConfig::pollInterval` until terminal or the
   configured entry timeout. On timeout, request cancellation.
4. If completed with a fill, monitor the target exit order and fetch quotes
   while waiting. If the stop, confirmed imbalance reversal, or maximum holding
   time is reached, cancel the target order and submit the corresponding exit.
5. Release slot and symbol when the attempt terminates.

Target/stop management is in-process; no broker-hosted OCO/bracket order is
created. Order quantity is sized from the risk manager's per-position cash
slot. Broker-specific short margin requirements and market impact are not
estimated by the share-count sizing helper.

## 8. Kite adapter details

`KiteConnectApi` implements the `KiteApi` interface:

| Operation | Kite API use |
|---|---|
| Quote refresh | Batched `/quote` requests; snapshots parsed from `last_price`, `average_price`, `depth`, `volume`, and `ohlc` |
| Fresh quote read | In-memory cache lookup with a five-second receipt-age limit |
| Backtest instrument lookup | `/instruments/NSE` CSV; resolve equity tokens and current tick sizes |
| Historical candles | `/instruments/historical/{instrument_token}/minute` with full-day request parameters |
| Live preflight | `/portfolio/positions` and `/orders`; block on existing MIS exposure for universe symbols |
| Cash | `/user/margins/equity`; free cash is validated and adjusted for utilization |
| Entry and exit | `/orders/regular` with MIS stop-limit buy and limit sell |
| Order status | `/orders/{order_id}` |
| Cancellation | Kite order-cancel endpoint |

HTTP, JSON, authentication, and broker errors are surfaced as exceptions.
Quote responses that omit an instrument are treated as unavailable instead of
causing a missing-key JSON exception.

## 9. Configuration and diagnostics

Runtime behavior is owned by `config.json`; changing only configuration does
not require recompiling the binary. `RuntimeConfig` maps the file into the
selection, decision, handling, risk, and backtest structures.
Startup logs report the selected run mode and active thresholds. Selection
emits periodic aggregate rejections. `Log.h` serializes concurrent messages.

The executable normally takes no arguments. The only supported optional switch
is `--config PATH`, which selects a different JSON file. Authentication policy,
short selling, run mode (`paper`, `shadow_live`, `live`, `backtest`, or
`optimize_100_days`), backtest date, and output path are configured in JSON.
Actual order submission additionally requires both `run.mode = "live"` and
`run.live_trading_confirmed = true`.

In `shadow_live`, snapshots, candidate decisions, simulated fills, exits, and
session metadata are written as JSON Lines to `run.data_output_path`. This is a
structured operational dataset and is not tied to a particular ML model.

## 10. Validation

Configure, build, and run tests:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`engine_tests` covers the 100-symbol universe, ownership transitions, event
waiting, selection filtering and price-change thresholds, decision signal
generation, portfolio sizing/cash calculations, paper handling, and an
end-to-end paper-mode pipeline. Backtest coverage includes synthesized replay,
entry fills, target/stop exits, and charge accounting.

## 11. Related documents

- [High-Level Design](./High%20Level%20Design.md)
