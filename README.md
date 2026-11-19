
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

The app gets NSE instrument tokens and tick sizes from Kite's instrument dump,
then requests minute candles for the configured NIFTY 100 symbols. Requests
are rate-spaced, so downloading a full day takes tens of seconds. A valid Kite
session and historical data for the requested trading day are required.
`--backtest` never submits live orders and cannot be combined with
`--enable-live-trading`.

This is an approximation, not an exact replay of live signals:

- OHLCV candles have no historical bid/ask spread, displayed depth, or
  five-level imbalance; those filters are omitted rather than fabricated.
- Signals are evaluated at a candle close and entries are tested against the
  next candle. If a candle touches both stop and target, the stop is assumed
  first. Positions still open at day end are closed at the final available
  candle close.
- The entry order is tested for one full next-candle interval, not the live
  30-second timeout. Position sizing starts from the requested capital, with
  up to ten simultaneous slots. P&L is gross; fees, taxes, and slippage are
  excluded.
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
