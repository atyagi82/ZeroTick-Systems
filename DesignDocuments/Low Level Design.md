# Low-Level Design

**Project:** ZeroTick-Systems  
**Design snapshot:** 2026-11-19  
**Language standard:** C++14

## 1. Code map

| Area | Main files | Responsibility |
|---|---|---|
| Application lifecycle | `src/main.cpp` | Parse flags, authenticate, select live/backtest mode, wire engines, poll quotes, shut down |
| Historical backtest | `include/zerotick/BacktestEngine.h`, `src/BacktestEngine.cpp` | Replay one-minute OHLCV with the available filters, budget model, and candle fill assumptions |
| Broker abstraction | `include/zerotick/KiteApi.h` | Market-data and order interface, Kite adapter declaration |
| Kite implementation | `src/KiteConnectApi.cpp` | OAuth callback, HTTP requests, quote cache, instrument master, historical candles, cash, order APIs |
| Domain and concurrency | `include/zerotick/Types.h`, `include/zerotick/EngineTypes.h`, `src/EngineTypes.cpp` | Snapshots, candidates, signals, NIFTY 100 universe, registry, thread-safe queue |
| Selection | `include/zerotick/StockSelectionEngine.h`, `src/StockSelectionEngine.cpp` | Consume quote events and filter candidates |
| Decision | `include/zerotick/TradingDecisionEngine.h`, `src/TradingDecisionEngine.cpp` | Re-check a candidate against a fresh quote and produce trade signals |
| Execution and risk | `include/zerotick/TradeHandlingEngine.h`, `src/TradeHandlingEngine.cpp`, `include/zerotick/Risk.h` | Reserve capacity, size, execute/paper-trade, monitor exits |
| Tests | `tests/EngineTests.cpp` | Engine pipeline, selection thresholds, risk sizing, and simulated order-flow tests |

## 2. Core data types

### `MarketSnapshot`

Defined in `Types.h`. A parsed quote carries:

- Symbol, last price, best bid/ask and up to five bid/ask levels.
- Aggregate displayed bid/ask quantities and cumulative volume.
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

1. `main` accepts `--login`, `--enable-live-trading`, `--enable-shorts`,
   `--config <path>`, `--backtest`, `--optimize-100-days`, and `--capital`;
   unknown flags fail startup. It loads and validates the JSON runtime
   configuration before authenticating. `allow_short_selling` in the file or
   `--enable-shorts` can opt in to short selling; live order submission still
   requires `--enable-live-trading`.
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

When `--backtest YYYY-MM-DD` is supplied, `main` branches before starting the
live quote poller or trading engines. It loads the NSE equity instrument dump
to resolve tokens and tick sizes, then requests one-minute candles for the
NIFTY 100 symbols from 09:15 to 15:30. Calls are spaced 400 ms apart. The
configured backtest capital defaults to 10,000; `--capital` overrides it.
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
| Relative volume must meet `minRelativeVolume` | 1.5 |
| Spread must be within `maxSpreadPercent` | 0.20% |
| Both displayed depth sides must meet `minDepthQuantity` | 1,000 |
| Price must move in the selected direction from the previous newer quote by at least `minPriceChangePercent` | 0.05% |
| Intraday range must meet the minimum | 0.30% |
| Intraday range and absolute price change must not exceed the maximum | 6.0% |

Relative volume uses an exponentially weighted average with `alpha = 0.2`.
The average is updated from cumulative volume snapshots before computing the
ratio, so the first observed volume has a relative volume of 1.0.

Price movement compares each valid quote with the previous newer quote for
that symbol. The first quote seeds the baseline and cannot qualify by itself.
The baseline advances even when a later selection filter rejects the quote;
out-of-order quote events cannot move it backward.

On rejection, selection releases the symbol. On success, it pushes a
`StockCandidate` and keeps ownership until decision claims it. Per-worker
summary logs are emitted at the configured five-second interval while quote
events are being processed; counts include selected symbols, stale events,
owned-symbol skips, and rejection reasons.

Short candidates require `allowShortSelling`; otherwise downward momentum is
rejected. The same setting is required independently by decision and handling,
so a short signal cannot reach an order submission path unless all stages were
explicitly configured. Runtime config sets all three guards; `--enable-shorts`
is an additional CLI opt-in.

## 5. Historical candle replay

`BacktestEngine` accepts historical candles grouped by symbol and replays
timestamps in ascending order. For each symbol it sums bar volume into
cumulative session volume, tracks running session highs/lows, and uses each
bar close as the selection price. The live selection volume, price-change,
and range gates are applied; spread and displayed-depth gates are omitted
because candle data has no order book. The decision relative-volume
confirmation is retained, but its spread and weighted-imbalance checks are
omitted. The decision non-falling-price recheck is effectively tautological:
the candle close serves as both the candidate price and the decision price.

A signal is formed at the bar close. Long entries require a later bar's high
to reach the trigger without opening above the limit; short entries require a
later bar's low to reach the trigger without opening below the limit. An
unfilled entry remains pending for `backtest.entry_timeout_minutes` (five
minutes by default), is checked on each subsequent minute candle, and is
canceled when it fills, times out, or the entry window closes. This is separate
from the live 30-second order timeout. Stop/target checks start after entry. If
a bar touches both, stop-loss is assumed first; gaps use the bar open as the
simulated fill. Remaining positions are closed at each symbol's final
available close.

The default backtest entry window is `[12:00, 13:00)` NSE time. Signals and
pending-entry fills are rejected outside this window; historical candles still
update price, volume, and range state. Existing positions continue to be
checked against their exits and are closed at end of day. The 100-day optimizer
uses the same window. This time filter is a proxy for avoiding the opening
period, not a direct measurement of volatility; live trading is unaffected.

The backtest reserves at most `backtest.max_open_positions` positions, with
each slot capped at `initialCapital / max_open_positions` and available cash.
The default cap is ten, so 10,000 initial capital yields 1,000 per slot. The
live/paper pipeline uses the separate `risk.max_open_positions` setting. This
is a concurrent-position limit, not a daily trade-count limit; the backtest
does not currently cap the number of sequential trades per day.
P&L is gross and excludes fees, taxes, and slippage.
The console table includes each closed trade's signal/entry/exit time, prices,
quantity, exit reason, and gross P&L. The final summary line prints total P&L.
Current NIFTY 100 membership is used for all dates, so this is not point-in-time
constituent data.

`--optimize-100-days` identifies the latest 100 completed sessions using the
first configured universe symbol, then downloads the universe's one-minute
candles in seven-calendar-day chunks. It runs each day separately so session
volume and open positions reset daily; the resulting capital compounds
between days. Twenty fixed variants cover five selection profiles and four
stop/target pairs. Variants are ranked on the first 70 sessions; the last 30
are reported separately as holdout results. Selection is based only on the
training period, not the holdout. The report includes the current defaults for
comparison and ends with the training-selected candidate's total 100-session
P&L. This remains an approximate gross simulation, not a guarantee of
profitability.

## 6. Decision worker

Each `TradingDecisionEngine` worker blocks on the candidate queue:

1. Claim `Selection -> Decision`.
2. Call `KiteApi::getQuote(candidate.symbol)`. The adapter rejects absent or
   cached snapshots older than five seconds; failures are logged and ownership
   is released.
3. Re-check the fresh spread against 0.10%.
4. If volume confirmation is enabled, require candidate relative volume of at
   least 1.2.
5. Compute weighted book imbalance using weights `1.0, 0.8, 0.6, 0.4, 0.2`
   for levels 1 through 5. Longs require at least `+0.05`; shorts require at
   most `-0.05` and must pass the short-selling opt-in.
6. Reject if price moves against the selected side since selection.
7. Infer tick size from adjacent depth prices, rounded to paise; use `0.01`
   when no step can be inferred.
8. For a long, place a buy stop-limit above the ask and set target/stop above
   and below entry. For a short, place a sell stop-limit below the bid and set
   target/stop below and above entry:

```text
long:  triggerBase = max(fresh.lastPrice, fresh.askPrice)
       trigger     = roundUpToTick(triggerBase, tick) + tick
       limit       = trigger + tick
       target/stop = limit * (1 +/- percentage / 100)

short: triggerBase = min(fresh.lastPrice, fresh.bidPrice)
       trigger     = roundDownToTick(triggerBase, tick) - tick
       limit       = trigger - tick
       target/stop = limit * (1 -/+ percentage / 100)
```

The defaults are a 0.30% target and 0.40% stop. The signal quantity is an
indicative cap derived from a fixed 50,000 budget divided by entry limit; final
quantity is recalculated by handling.

On a rejected decision, release `Decision -> Unassigned`. On approval, enqueue
the signal and keep ownership until handling claims it.

## 7. Handling and portfolio risk

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
symbol. No Kite orders are submitted. For live positions, a long enters with a
buy and exits with a sell; a short enters with a sell and exits with a buy to
cover. The `allowShortSelling` handling guard is a final check before order
submission. Broker-specific short margin requirements are not estimated by the
share-count sizing helper.

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
created. Order quantity is sized from the risk manager's per-position cash
slot; the current handling path does not request a broker margin estimate.

In the current `main`, `PortfolioRiskManager` is initialized from live startup
cash only when live mode is enabled. In paper mode that value remains zero,
which means the actual CLI paper path cannot reserve a slot. The paper-mode
tests construct the risk manager with a nonzero budget, so they do not cover
this CLI configuration gap.

## 8. Kite adapter details

`KiteConnectApi` implements the `KiteApi` interface:

| Operation | Kite API use |
|---|---|
| Quote refresh | Batched `/quote` requests; snapshots parsed from `last_price`, `depth`, `volume`, and `ohlc` |
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
- `--backtest YYYY-MM-DD`: fetch and replay one-minute candles without sending
  orders.
- `--optimize-100-days`: download and analyze the latest 100 completed sessions
  using the bounded parameter sweep.
- `--capital AMOUNT`: set backtest starting capital (default 10,000).

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
end-to-end paper-mode pipeline. It also checks next-bar backtest fills,
target exits, and conservative stop-first handling for ambiguous candles.

## 11. Related documents

- [High-Level Design](./High%20Level%20Design.md)
