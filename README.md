
- **Language:** C++
- **Broker integration:** Zerodha Kite Connect APIs
- **Focus:** Market data processing, strategy orchestration, order execution, and risk management

## Development setup

The initial Kite transport uses Windows WinHTTP/CNG, so it currently builds on Windows with CMake and C++14 or newer. CMake downloads the header-only `nlohmann/json` dependency during configuration.

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run the application with:

```powershell
.\build\Release\zerotick.exe
```

Live order mode is opt-in and sends real Kite orders. Do not use it until you have reviewed its limitations below:

```powershell
.\build\Release\zerotick.exe --enable-live-trading
```

## Shadow live simulation and structured data

Shadow live mode runs the normal live quote, selection, decision, risk, entry,
and exit pipeline without submitting broker orders. It waits for later live
quotes to cross each stop-limit entry, target, or stop instead of using the
instant-fill behavior of the lightweight paper mode:

```powershell
.\build\Release\zerotick.exe --shadow-live
```

Structured events are appended to `shadow_live_events.jsonl` by default. Use
an alternate path when keeping separate sessions or experiments:

```powershell
.\build\Release\zerotick.exe --shadow-live --data-output .\data\session-01.jsonl
```

The output is JSON Lines: every line is an independent JSON object and is
flushed immediately. Each object has `schema_version`, `session_id`,
`event_id`, `event_type`, and `event_time_utc`. Candidate and signal IDs join
events across pipeline stages. Event types include `session_started`,
`market_snapshot`, `selection_decision`, `trade_decision`, `entry_pending`,
`entry_filled`, `entry_expired`, `entry_rejected`, `position_closed`, and
`position_interrupted`. Market events retain the five bid and ask levels,
prices, displayed quantities, volume, and OHLC fields. Decision events retain
the computed strategy features and planned entry, target, and stop values.

The console updates a compact status line with snapshot, candidate, signal,
pending-entry, open-position, and completed-trade counts. Press Ctrl+C for an
orderly shutdown. Open simulated positions are recorded as
`position_interrupted`; they are not invented as completed outcomes.

Entry fills use the first fresh quote after the signal whose last price has
crossed the trigger and whose executable ask (long) or bid (short) remains
inside the limit. Entries expire after `handling.entry_fill_timeout_seconds`.
Long exits use the bid and short exits use the ask after the live last price
reaches the target or stop. This is a quote-driven simulation: it cannot prove
queue position or that the displayed quantity would fill the complete order.
`--shadow-live` and `--enable-live-trading` are mutually exclusive.

To force a fresh Kite login and token exchange:

```powershell
.\build\Release\zerotick.exe --login
```

The app opens the login URL in your browser and listens only on `127.0.0.1:8765` for the configured `/callback`. After you sign in, it captures the one-time request token locally, exchanges it for an access token, and stores the access token in `.env` without printing either token. Configure the Kite app redirect URL as `http://127.0.0.1:8765/callback`. A Kite API key by itself cannot create an access token; the browser login step is required.

The quote poller refreshes the configured NIFTY 100 universe in batches of 40
approximately once per second. It publishes available snapshots to selection.
Decision workers re-read a fresh cached quote before emitting a trade signal,
and handling workers apply portfolio risk checks before paper or live
execution. Selection logs aggregate results instead of printing one line per
quote.

## Algorithm configuration

The application automatically loads `config.json` from the repository working
directory at startup, including for backtests. Run the executable from the
repository root to use the checked-in config and local `.env`:

```powershell
.\build\Release\zerotick.exe --backtest 2026-11-18
```

Edit `config.json` and restart the executable for changes to take effect; no
rebuild is required. Settings are read once at startup and are not hot-reloaded.
An alternate file can be selected with `--config`:

```powershell
.\build\Release\zerotick.exe --config .\my-config.json --backtest 2026-11-18
```

The config contains market-data polling, quote freshness and fallback tick
size; selection thresholds and volume smoothing; decision filters, target and
stop percentages, order-book weights and entry tick offsets; worker counts and
order timeouts; live/paper risk limits and cash; and backtest capital, its
independent position cap, and entry window. `risk.max_open_positions` applies
to the live/paper pipeline; `backtest.max_open_positions` controls historical
position sizing. Each backtest slot is capped at
`backtest.initial_capital / backtest.max_open_positions`. Defaults include a
0.30% target, 0.40% stop, INR 10,000 backtest capital, and a backtest-only
new-entry window of 12:00–13:00 NSE time. The compiled fallback is 10
backtest slots; the checked-in config sets one slot so the full INR 10,000 can
size a trade. `risk.paper_trading_cash` does not change backtest capital.
Backtest entry orders remain eligible for five minutes by default
(`backtest.entry_timeout_minutes`), then expire if unfilled. This controls the
entry order only; it does not set a minimum or maximum holding period for a
filled position. `backtest.max_open_positions` limits simultaneous positions,
not total trades per day; there is no daily trade-count cap.
Unknown keys and invalid values stop startup with an error.

Runtime and backtest diagnostics are appended to `logs.txt` in the working
directory, with timestamps. The log records API operation names, HTTP status,
response size and duration, plus backtest filter rejections, risk decisions,
and entry/fill outcomes. It does not record API credentials, access tokens, or
HTTP request/response bodies. `logs.txt` is local and ignored by Git.

`--capital <amount>` overrides `backtest.initial_capital` for a backtest or
100-day optimization run. Set `allow_short_selling` to `true` in the config or
pass `--enable-shorts` to enable short entries. Live orders still require the
separate `--enable-live-trading` flag.

## Historical candle backtest

Run a one-minute OHLCV replay for a trading day. Capital defaults to ₹10,000,
and closed trades are printed in an aligned console table. The final line
reports total profit or loss:

```powershell
.\build\Release\zerotick.exe --backtest 2026-11-18
```

Override the starting capital with:

```powershell
.\build\Release\zerotick.exe --backtest 2026-11-18 --capital 10000
```

The historical simulator is long-only by default. To include short entries
and buy-to-cover exits, opt in through `allow_short_selling` in `config.json`
or pass:

```powershell
.\build\Release\zerotick.exe --backtest 2026-11-18 --capital 10000 --enable-shorts
```

For a parameter study over the latest 100 completed trading sessions, run:

```powershell
.\build\Release\zerotick.exe --optimize-100-days
```

The study tests 20 predefined combinations of selection thresholds and
stop/target settings. It ranks candidates on the first 70 sessions and reports
the last 30 separately as holdout data. It downloads weekly candle ranges for
the current NIFTY 100 universe; expect a long run due to Kite's historical API
rate limit. The delimited report compares the current defaults and ends with
the training-selected candidate's total P&L across all 100 sessions.
`--capital` can be used to change the starting balance.
Add `--enable-shorts` to include the same short strategy in the study.

The app gets NSE instrument tokens and tick sizes from Kite's instrument dump,
then requests minute candles for the configured NIFTY 100 symbols. Requests
are rate-spaced, so downloading a full day takes tens of seconds. A valid Kite
session and historical data for the requested trading day are required.
`--backtest` never submits live orders and cannot be combined with
`--enable-live-trading`.

Historical backtests (including the 100-day study) only open new positions
during the NSE-time window **12:00–13:00**. Signals and entry fills outside
that window are ignored; positions opened inside it continue to be monitored
for their target/stop and end-of-day exit. This is a time-of-day filter, not a
measured volatility detector, and does not change live trading.

Live short selling is disabled by default. Enable it through the config or
`--enable-shorts`; live order submission still separately requires
`--enable-live-trading`. Short MIS orders sell to enter and buy to cover at the
target or stop. Broker margin/eligibility checks still apply; the position-size
cap does not model broker-specific short margin requirements.

This is an approximation, not an exact replay of live signals:

- OHLCV candles have no historical bid/ask spread, displayed depth, or
  five-level imbalance; those filters are omitted rather than fabricated.
- Signals are evaluated at a candle close and entries are tested against the
  following one-minute candles until the configured entry timeout. If a candle
  touches both stop and target, the stop is assumed first. Positions still
  open at day end are closed at the final available candle close.
- Position sizing starts from the requested capital with the configured number
  of simultaneous slots. P&L is gross; fees, taxes, and slippage are excluded.
- The replay uses the checked-in current NIFTY 100 list, not historical index
  membership, so earlier dates can have survivorship bias.
- Optimization results are gross and exclude charges. A positive holdout
  result is not a guarantee of future performance.

The table shows each closed trade's symbol, signal/entry/exit time, quantity,
entry/exit price, gross P&L, and exit reason. The summary includes return,
drawdown, fills, and win rate. Candle replay cannot establish the live
strategy's actual fills or profitability.

### Local Kite credentials

Copy `.env.example` to `.env` and fill in the API key and secret locally. `.env` is excluded by `.gitignore`; never commit it or paste its contents into chat. When no access token is present, or when launched with `--login`, the app captures a fresh request token through the local browser callback, exchanges it, and stores the access token in `.env`. The request token is kept in memory only. Access tokens are sensitive; revoke/regenerate them when needed. Run the program from the repository root so it finds `.env`.
