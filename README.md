
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

Execution behavior is selected by `run.mode` in `config.json`. Supported modes
are `paper`, `shadow_live`, `backtest`, `optimize_100_days`, and `live`. Live
mode additionally requires `run.live_trading_confirmed` to be `true`.

## Shadow live simulation and structured data

Shadow live mode runs the normal live quote, selection, decision, risk, entry,
and exit pipeline without submitting broker orders. It waits for later live
quotes to cross each stop-limit entry, target, or stop instead of using the
instant-fill behavior of the lightweight paper mode:

Set `run.mode` to `shadow_live`, then run the executable without mode flags.

Structured events are appended to `shadow_live_events.jsonl` by default. Use
an alternate path when keeping separate sessions or experiments:

Set `run.data_output_path` to choose a different JSONL destination.

Set `run.data_output_max_mb` to rotate structured output before the next
complete JSON record would exceed that size. A path such as
`data/shadow/2026-11-24.jsonl` continues as
`2026-11-24.part002.jsonl`, `part003`, and so on. Rotation never splits a JSON
record, and every part retains the same session ID so the ML loader can ingest
the files as one session.

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
The single configured run mode prevents shadow and real live trading from
being enabled together.

Set `run.force_login` to `true` to force a fresh Kite login and token exchange.

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

Set `run.mode` to `backtest` and set `run.backtest_date`, then run the
executable without arguments.

Edit `config.json` and restart the executable for changes to take effect; no
rebuild is required. Settings are read once at startup and are not hot-reloaded.
An alternate file can be selected with `--config`:

```powershell
.\build\Release\zerotick.exe --config .\my-config.json
```

The config contains run mode, authentication behavior, market-data polling,
quote freshness and fallback tick
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

Set `backtest.initial_capital` to choose historical starting capital and
`allow_short_selling` to enable short entries. Real orders require both
`run.mode: "live"` and `run.live_trading_confirmed: true`.

## Historical candle backtest

Run an OHLCV replay for a trading day. By default, every historical minute is
expanded into 60 deterministic virtual one-second bars before it reaches the
strategy. Capital defaults to ₹10,000, and closed trades are printed in an
aligned console table. The final line reports total profit or loss:

```json
"run": {
  "mode": "backtest",
  "force_login": false,
  "backtest_date": "2026-11-18"
}
```

Change `backtest.initial_capital` to select the starting capital.

The historical simulator is long-only by default. Set
`allow_short_selling` to `true` to include short entries and buy-to-cover
exits.

For a parameter study over the latest 100 completed trading sessions, run:

Set `run.mode` to `optimize_100_days` and run the executable.

The study tests 20 predefined combinations of selection thresholds and
stop/target settings. It ranks candidates on the first 70 sessions and reports
the last 30 separately as holdout data. It downloads weekly candle ranges for
the current NIFTY 100 universe; expect a long run due to Kite's historical API
rate limit. The delimited report compares the current defaults and ends with
the training-selected candidate's total P&L across all 100 sessions.
Use `backtest.initial_capital` to change the starting balance and
`allow_short_selling` to include the short strategy.

The app gets NSE instrument tokens and tick sizes from Kite's instrument dump,
then requests minute candles for the configured NIFTY 100 symbols. Requests
are rate-spaced, so downloading a full day takes tens of seconds. A valid Kite
session and historical data for the requested trading day are required.
Backtest mode never submits live orders.

Historical backtests (including the 100-day study) only open new positions
during the NSE-time window **12:00–13:00**. Signals and entry fills outside
that window are ignored; positions opened inside it continue to be monitored
for their target/stop and end-of-day exit. This is a time-of-day filter, not a
measured volatility detector, and does not change live trading.

Live short selling is disabled by default. Enable it through
`allow_short_selling`; live order submission still separately requires the
confirmed `live` run mode. Short MIS orders sell to enter and buy to cover at the
target or stop. Broker margin/eligibility checks still apply; the position-size
cap does not model broker-specific short margin requirements.

The one-second generator preserves each source candle's exact open, high, low,
close, total volume, and time range. It creates a reproducible path through the
high and low, adds bounded deterministic movement between those points, and
distributes the candle volume across its 60 generated seconds. Backtest entry
timeouts and the entry window use virtual market seconds; replay does not
sleep, so a full session still runs as quickly as the machine permits.

Historical candles do not contain bid and ask prices. Backtest execution uses
an estimated spread equal to `synthetic_base_spread_percent` plus the source
candle's range percentage multiplied by
`synthetic_volatility_spread_factor`, capped by
`synthetic_maximum_spread_percent`. Long fills and short exits use the
estimated ask; short fills and long exits use the estimated bid. Set
`synthesize_one_second_quotes` to `false` to retain the original minute-bar
replay for comparisons.

This remains an approximation, not an exact replay of live signals:

- OHLCV candles have no historical displayed depth or five-level imbalance;
  those filters remain omitted. The estimated spread is a configurable model,
  not observed historical bid/ask data.
- Signals are evaluated at generated one-second closes and entries are tested
  against later generated seconds until the configured entry timeout. If a
  generated bar touches both stop and target, the stop is assumed first.
  Positions still open at day end are closed at the final available close.
- The true price and volume sequence inside each source minute is unknowable.
  The generated path is reproducible but represents only one plausible path.
- Position sizing starts from the requested capital with the configured number
  of simultaneous slots. Configured brokerage and statutory charges are
  deducted; additional slippage remains excluded.
- The replay uses the checked-in current NIFTY 100 list, not historical index
  membership, so earlier dates can have survivorship bias.
- Optimization results include configured charges. A positive holdout result
  is not a guarantee of future performance.

The table shows each closed trade's symbol, signal/entry/exit time, quantity,
entry/exit price, gross P&L, and exit reason. The summary includes return,
drawdown, fills, and win rate. Candle replay cannot establish the live
strategy's actual fills or profitability.

## Shadow-depth ML research

The optional Python subsystem under `ml/` implements three offline phases:
Optuna strategy tuning over recorded five-level depth, XGBoost meta-label
training for candidate acceptance, and model-assisted Optuna tuning. It reads
schema-v1 shadow-live JSONL and writes reviewable artifacts without modifying
`config.json` or enabling live orders.

Use 64-bit Python 3.10–3.12 and see [ml/README.md](ml/README.md) for setup,
data requirements, commands, artifacts, and validation limitations.

## Historical backtest ML dataset

The historical workflow mirrors the same three research phases without
pretending that minute candles contain live depth. It exports the latest 200
completed trading sessions as schema-v1 JSONL: the older 100 are permanently
marked `train`, and the following 100 are marked `validation`. Each record
contains one symbol/session and its ordered minute OHLCV candles.

After building the Release binary, collect the data with:

```powershell
.\scripts\collect_backtest_ml_data.ps1 -ForceLogin
```

The script uses a temporary config and does not alter `config.json`.
Collection is API-rate-limited and can take a long time. Then run:

```powershell
python ml\run_backtest_pipeline.py validate
python ml\run_backtest_pipeline.py all
```

Optuna and XGBoost use only the first 100 sessions. The second 100-session
period is scored once after all parameters and the model are frozen.

### Local Kite credentials

Copy `.env.example` to `.env` and fill in the API key and secret locally. `.env` is excluded by `.gitignore`; never commit it or paste its contents into chat. When no access token is present, or when `run.force_login` is true, the app captures a fresh request token through the local browser callback, exchanges it, and stores the access token in `.env`. The request token is kept in memory only. Access tokens are sensitive; revoke/regenerate them when needed. Run the program from the repository root so it finds `.env`.
