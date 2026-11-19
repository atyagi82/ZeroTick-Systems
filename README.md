# ZeroTick-Systems

ZeroTick-Systems is a C++ algorithmic trading platform being developed using Zerodha's Kite developer APIs. The project aims to support fast, data-driven trading strategies and execution that seek small but frequent opportunities while maintaining strict risk controls.

> **Status:** Prototype. Kite Connect login, batched quote retrieval, worker status reporting, and focused strategy unit tests have been exercised locally. Market-hours validation and production risk controls are still pending. Live order placement is disabled.

## Current status and validation

Validated locally on 2026-11-19:

- The browser login callback completed and the app exchanged the request token for an access token.
- The Kite quote endpoint returned a batch for all 20 configured instruments.
- All 20 workers started and printed market snapshots, depth totals, indicator values, and `NO_ENTRY` reasons.
- The C++ application built successfully with Visual Studio 2022 and CMake.

This run was outside market hours. The returned snapshots showed one-sided or unavailable book levels and repeated prices; this confirms API retrieval, not live, actionable order-book depth. No entry signal fired and no orders were placed. A market-hours validation and paper-trading/backtest evaluation remain necessary.

### Current live-data test mode

Only one of the 20 stock worker threads is enabled: the first configured symbol, `RELIANCE`. The other 19 stock workers are disabled. The quote poller still fetches the configured 20-symbol batch once per second, while only the `RELIANCE` worker evaluates and logs strategy signals. Live order placement remains disabled.

## Project goals

- Connect to Zerodha Kite for market data and order management
- Execute low-latency, high-frequency-oriented strategies
- Target frequent trades with small per-trade profit objectives
- Support configurable risk limits and position sizing
- Provide observability through structured logs and trading metrics
- Favor deterministic, testable C++ components

## Technology

- **Language:** C++
- **Broker integration:** Zerodha Kite Connect APIs
- **Focus:** Market data processing, strategy orchestration, order execution, and risk management

## Intended capabilities

- Real-time instrument and market-data subscriptions
- Strategy lifecycle management
- Entry and exit signal generation
- Paper trading and backtesting
- Order validation and retry handling
- Daily loss limits, maximum position limits, and kill-switch controls
- Performance reporting and audit logs

## Safety and limitations

Trading involves substantial financial risk. Strategies targeting high-frequency or low-profit trades can be disproportionately affected by fees, slippage, market impact, latency, API limits, and operational failures.

Before live trading:

1. Test strategies extensively with historical and replayed market data.
2. Run in a paper-trading or sandbox environment.
3. Validate broker API behavior and order lifecycle handling.
4. Add exchange and broker-compliant risk controls.
5. Start with minimal capital and monitor the system continuously.

This project does not guarantee profits or suitability for any trading strategy.

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

To force a fresh Kite login and token exchange:

```powershell
.\build\Release\zerotick.exe --login
```

The app opens the login URL in your browser and listens only on `127.0.0.1:8765` for the configured `/callback`. After you sign in, it captures the one-time request token locally, exchanges it for an access token, and stores the access token in `.env` without printing either token. Configure the Kite app redirect URL as `http://127.0.0.1:8765/callback`. A Kite API key by itself cannot create an access token; the browser login step is required.

The executable authenticates with Kite Connect and fetches the 20-symbol quote/depth batch once per second. In the current test mode, only the `RELIANCE` worker processes its cached snapshot; the other 19 stock workers are disabled. It does not place live orders.

The active worker writes a synchronized status line showing symbol, last price, best bid/ask, five-level depth totals, weighted imbalance, spread, price change, confirmation count, quote age, signal, and the first unmet entry/exit condition. Expect roughly one status line per second; quote/API failures are also logged.

### Local Kite credentials

Copy `.env.example` to `.env` and fill in the API key and secret locally. `.env` is excluded by `.gitignore`; never commit it or paste its contents into chat. When no access token is present, or when launched with `--login`, the app captures a fresh request token through the local browser callback, exchanges it, and stores the access token in `.env`. The request token is kept in memory only. Access tokens are sensitive; revoke/regenerate them when needed. Run the program from the repository root so it finds `.env`.

## Code structure

- `include/zerotick/` - Public C++ interfaces and domain types
- `src/` - State machine, worker, strategy, thread pool, and Kite Connect REST adapter
- `tests/` - Focused strategy behavior tests
- `CMakeLists.txt` - Library and executable build configuration

Each worker owns exactly one symbol and follows a state machine:

```text
Idle -> Monitoring -> EntryPending -> PositionOpen -> ExitPending -> Cooldown -> Monitoring
                  \-> Stopped / Error
```

## Basic trading algorithm

`OrderImbalanceStrategy` is a long-only heuristic; it does not calculate or promise a probability of success.

For Kite's five bid and ask levels, the strategy applies weights `1.0, 0.8, 0.6, 0.4, 0.2`, giving nearer liquidity more influence. It enters only when:

- Both sides of the order book are available and valid.
- Weighted imbalance `(weighted bids - weighted asks) / (weighted bids + weighted asks)` is at least `+0.25`.
- The best bid/ask spread is no more than `0.10%` of the midpoint.
- Last traded price is not lower than the previous one-second snapshot.
- All conditions hold for three consecutive snapshots.

It exits at a `+0.20%` price move from entry or a `-0.40%` move. Before fees and slippage, that target/stop ratio requires a win rate above approximately `66.7%` to break even. This is not a forecast; success probability must be measured using historical order-book data and realistic transaction costs. Live order placement remains explicitly disabled in the Kite adapter.


## 19th November First Testing

Release\zerotick.exe --login
Starting Kite login in your browser...
Waiting for the Kite login callback on localhost...
Kite login callback received; exchanging token.
Kite access token obtained and saved in local .env
[kite] quote_batch=received instruments=20
Started 20 stock worker threads
[worker] symbol=NTPC state=Monitoring last=320.40 bid=0.00 ask=320.40 bid_depth_qty=0 ask_depth_qty=2169 imbalance=-1.000 ema_fast=320.40 ema_slow=320.40 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=ULTRACEMCO state=Monitoring last=12428.00 bid=0.00 ask=12428.00 bid_depth_qty=0 ask_depth_qty=274 imbalance=-1.000 ema_fast=12428.00 ema_slow=12428.00 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=ONGC state=Monitoring last=225.00 bid=0.00 ask=225.00 bid_depth_qty=0 ask_depth_qty=20951 imbalance=-1.000 ema_fast=225.00 ema_slow=225.00 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=TITAN state=Monitoring last=4648.10 bid=4648.10 ask=0.00 bid_depth_qty=21 ask_depth_qty=0 imbalance=1.000 ema_fast=4648.10 ema_slow=4648.10 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=MARUTI state=Monitoring last=12632.00 bid=0.00 ask=12632.00 bid_depth_qty=0 ask_depth_qty=18 imbalance=-1.000 ema_fast=12632.00 ema_slow=12632.00 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=SUNPHARMA state=Monitoring last=1871.50 bid=0.00 ask=1871.50 bid_depth_qty=0 ask_depth_qty=38 imbalance=-1.000 ema_fast=1871.50 ema_slow=1871.50 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=M&M state=Monitoring last=3359.10 bid=0.00 ask=3359.10 bid_depth_qty=0 ask_depth_qty=1139 imbalance=-1.000 ema_fast=3359.10 ema_slow=3359.10 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=BAJFINANCE state=Monitoring last=1093.85 bid=0.00 ask=1093.85 bid_depth_qty=0 ask_depth_qty=1564 imbalance=-1.000 ema_fast=1093.85 ema_slow=1093.85 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=KOTAKBANK state=Monitoring last=494.85 bid=0.00 ask=494.85 bid_depth_qty=0 ask_depth_qty=5881 imbalance=-1.000 ema_fast=494.85 ema_slow=494.85 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=HINDUNILVR state=Monitoring last=1993.00 bid=1993.00 ask=0.00 bid_depth_qty=45 ask_depth_qty=0 imbalance=1.000 ema_fast=1993.00 ema_slow=1993.00 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=AXISBANK state=Monitoring last=1360.00 bid=0.00 ask=1360.00 bid_depth_qty=0 ask_depth_qty=2255 imbalance=-1.000 ema_fast=1360.00 ema_slow=1360.00 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=LT state=Monitoring last=4159.00 bid=0.00 ask=4159.00 bid_depth_qty=0 ask_depth_qty=835 imbalance=-1.000 ema_fast=4159.00 ema_slow=4159.00 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=ITC state=Monitoring last=296.60 bid=296.60 ask=0.00 bid_depth_qty=1707 ask_depth_qty=0 imbalance=1.000 ema_fast=296.60 ema_slow=296.60 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=SBIN state=Monitoring last=1135.00 bid=0.00 ask=1135.00 bid_depth_qty=0 ask_depth_qty=2961 imbalance=-1.000 ema_fast=1135.00 ema_slow=1135.00 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1
[worker] symbol=BHARTIARTL state=Monitoring last=2132.00 bid=0.00 ask=2132.00 bid_depth_qty=0 ask_depth_qty=2097 imbalance=-1.000 ema_fast=2132.00 ema_slow=2132.00 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=0
[worker] symbol=TCS state=Monitoring last=2379.50 bid=2379.50 ask=0.00 bid_depth_qty=666 ask_depth_qty=0 imbalance=1.000 ema_fast=2379.50 ema_slow=2379.50 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=0
[worker] symbol=INFY state=Monitoring last=1108.80 bid=0.00 ask=1108.80 bid_depth_qty=0 ask_depth_qty=2111 imbalance=-1.000 ema_fast=1108.80 ema_slow=1108.80 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=0
[worker] symbol=ICICIBANK state=Monitoring last=1601.30 bid=1601.30 ask=0.00 bid_depth_qty=7004 ask_depth_qty=0 imbalance=1.000 ema_fast=1601.30 ema_slow=1601.30 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=0
[worker] symbol=RELIANCE state=Monitoring last=1363.00 bid=0.00 ask=1363.00 bid_depth_qty=0 ask_depth_qty=5121 imbalance=-1.000 ema_fast=1363.00 ema_slow=1363.00 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=0
[worker] symbol=HDFCBANK state=Monitoring last=808.15 bid=0.00 ask=808.15 bid_depth_qty=0 ask_depth_qty=4942 imbalance=-1.000 ema_fast=808.15 ema_slow=808.15 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=0
[worker] symbol=NTPC state=Monitoring last=320.40 bid=0.00 ask=320.40 bid_depth_qty=0 ask_depth_qty=2169 imbalance=-1.000 ema_fast=320.40 ema_slow=320.40 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=35
[worker] symbol=LT state=Monitoring last=4159.00 bid=0.00 ask=4159.00 bid_depth_qty=0 ask_depth_qty=835 imbalance=-1.000 ema_fast=4159.00 ema_slow=4159.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=35
[worker] symbol=AXISBANK state=Monitoring last=1360.00 bid=0.00 ask=1360.00 bid_depth_qty=0 ask_depth_qty=2255 imbalance=-1.000 ema_fast=1360.00 ema_slow=1360.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=35
[worker] symbol=HINDUNILVR state=Monitoring last=1993.00 bid=1993.00 ask=0.00 bid_depth_qty=45 ask_depth_qty=0 imbalance=1.000 ema_fast=1993.00 ema_slow=1993.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=36
[worker] symbol=KOTAKBANK state=Monitoring last=494.85 bid=0.00 ask=494.85 bid_depth_qty=0 ask_depth_qty=5881 imbalance=-1.000 ema_fast=494.85 ema_slow=494.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=36
[worker] symbol=BAJFINANCE state=Monitoring last=1093.85 bid=0.00 ask=1093.85 bid_depth_qty=0 ask_depth_qty=1564 imbalance=-1.000 ema_fast=1093.85 ema_slow=1093.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=36
[worker] symbol=M&M state=Monitoring last=3359.10 bid=0.00 ask=3359.10 bid_depth_qty=0 ask_depth_qty=1139 imbalance=-1.000 ema_fast=3359.10 ema_slow=3359.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=36
[worker] symbol=SUNPHARMA state=Monitoring last=1871.50 bid=0.00 ask=1871.50 bid_depth_qty=0 ask_depth_qty=38 imbalance=-1.000 ema_fast=1871.50 ema_slow=1871.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=36
[worker] symbol=MARUTI state=Monitoring last=12632.00 bid=0.00 ask=12632.00 bid_depth_qty=0 ask_depth_qty=18 imbalance=-1.000 ema_fast=12632.00 ema_slow=12632.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=36
[worker] symbol=TITAN state=Monitoring last=4648.10 bid=4648.10 ask=0.00 bid_depth_qty=21 ask_depth_qty=0 imbalance=1.000 ema_fast=4648.10 ema_slow=4648.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=36
[worker] symbol=ONGC state=Monitoring last=225.00 bid=0.00 ask=225.00 bid_depth_qty=0 ask_depth_qty=20951 imbalance=-1.000 ema_fast=225.00 ema_slow=225.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=36
[worker] symbol=ULTRACEMCO state=Monitoring last=12428.00 bid=0.00 ask=12428.00 bid_depth_qty=0 ask_depth_qty=274 imbalance=-1.000 ema_fast=12428.00 ema_slow=12428.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=36
[worker] symbol=HDFCBANK state=Monitoring last=808.15 bid=0.00 ask=808.15 bid_depth_qty=0 ask_depth_qty=4942 imbalance=-1.000 ema_fast=808.15 ema_slow=808.15 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=44
[worker] symbol=RELIANCE state=Monitoring last=1363.00 bid=0.00 ask=1363.00 bid_depth_qty=0 ask_depth_qty=5121 imbalance=-1.000 ema_fast=1363.00 ema_slow=1363.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=44
[worker] symbol=ICICIBANK state=Monitoring last=1601.30 bid=1601.30 ask=0.00 bid_depth_qty=7004 ask_depth_qty=0 imbalance=1.000 ema_fast=1601.30 ema_slow=1601.30 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=44
[worker] symbol=INFY state=Monitoring last=1108.80 bid=0.00 ask=1108.80 bid_depth_qty=0 ask_depth_qty=2111 imbalance=-1.000 ema_fast=1108.80 ema_slow=1108.80 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=44
[worker] symbol=TCS state=Monitoring last=2379.50 bid=2379.50 ask=0.00 bid_depth_qty=666 ask_depth_qty=0 imbalance=1.000 ema_fast=2379.50 ema_slow=2379.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=44
[worker] symbol=SBIN state=Monitoring last=1135.00 bid=0.00 ask=1135.00 bid_depth_qty=0 ask_depth_qty=2961 imbalance=-1.000 ema_fast=1135.00 ema_slow=1135.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=44
[worker] symbol=ITC state=Monitoring last=296.60 bid=296.60 ask=0.00 bid_depth_qty=1707 ask_depth_qty=0 imbalance=1.000 ema_fast=296.60 ema_slow=296.60 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=44
[worker] symbol=BHARTIARTL state=Monitoring last=2132.00 bid=0.00 ask=2132.00 bid_depth_qty=0 ask_depth_qty=2097 imbalance=-1.000 ema_fast=2132.00 ema_slow=2132.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=44
[worker] symbol=ULTRACEMCO state=Monitoring last=12428.00 bid=0.00 ask=12428.00 bid_depth_qty=0 ask_depth_qty=274 imbalance=-1.000 ema_fast=12428.00 ema_slow=12428.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1044
[worker] symbol=ONGC state=Monitoring last=225.00 bid=0.00 ask=225.00 bid_depth_qty=0 ask_depth_qty=20951 imbalance=-1.000 ema_fast=225.00 ema_slow=225.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1044
[worker] symbol=TITAN state=Monitoring last=4648.10 bid=4648.10 ask=0.00 bid_depth_qty=21 ask_depth_qty=0 imbalance=1.000 ema_fast=4648.10 ema_slow=4648.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1044
[worker] symbol=SUNPHARMA state=Monitoring last=1871.50 bid=0.00 ask=1871.50 bid_depth_qty=0 ask_depth_qty=38 imbalance=-1.000 ema_fast=1871.50 ema_slow=1871.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1044
[worker] symbol=MARUTI state=Monitoring last=12632.00 bid=0.00 ask=12632.00 bid_depth_qty=0 ask_depth_qty=18 imbalance=-1.000 ema_fast=12632.00 ema_slow=12632.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1044
[worker] symbol=M&M state=Monitoring last=3359.10 bid=0.00 ask=3359.10 bid_depth_qty=0 ask_depth_qty=1139 imbalance=-1.000 ema_fast=3359.10 ema_slow=3359.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1044
[worker] symbol=BAJFINANCE state=Monitoring last=1093.85 bid=0.00 ask=1093.85 bid_depth_qty=0 ask_depth_qty=1564 imbalance=-1.000 ema_fast=1093.85 ema_slow=1093.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1044
[worker] symbol=KOTAKBANK state=Monitoring last=494.85 bid=0.00 ask=494.85 bid_depth_qty=0 ask_depth_qty=5881 imbalance=-1.000 ema_fast=494.85 ema_slow=494.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1044
[worker] symbol=AXISBANK state=Monitoring last=1360.00 bid=0.00 ask=1360.00 bid_depth_qty=0 ask_depth_qty=2255 imbalance=-1.000 ema_fast=1360.00 ema_slow=1360.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1044
[worker] symbol=HINDUNILVR state=Monitoring last=1993.00 bid=1993.00 ask=0.00 bid_depth_qty=45 ask_depth_qty=0 imbalance=1.000 ema_fast=1993.00 ema_slow=1993.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1044
[worker] symbol=LT state=Monitoring last=4159.00 bid=0.00 ask=4159.00 bid_depth_qty=0 ask_depth_qty=835 imbalance=-1.000 ema_fast=4159.00 ema_slow=4159.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1044
[worker] symbol=NTPC state=Monitoring last=320.40 bid=0.00 ask=320.40 bid_depth_qty=0 ask_depth_qty=2169 imbalance=-1.000 ema_fast=320.40 ema_slow=320.40 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1044
[worker] symbol=BHARTIARTL state=Monitoring last=2132.00 bid=0.00 ask=2132.00 bid_depth_qty=0 ask_depth_qty=2097 imbalance=-1.000 ema_fast=2132.00 ema_slow=2132.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1059
[worker] symbol=ITC state=Monitoring last=296.60 bid=296.60 ask=0.00 bid_depth_qty=1707 ask_depth_qty=0 imbalance=1.000 ema_fast=296.60 ema_slow=296.60 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1059
[worker] symbol=SBIN state=Monitoring last=1135.00 bid=0.00 ask=1135.00 bid_depth_qty=0 ask_depth_qty=2961 imbalance=-1.000 ema_fast=1135.00 ema_slow=1135.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1059
[worker] symbol=TCS state=Monitoring last=2379.50 bid=2379.50 ask=0.00 bid_depth_qty=666 ask_depth_qty=0 imbalance=1.000 ema_fast=2379.50 ema_slow=2379.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1059
[worker] symbol=INFY state=Monitoring last=1108.80 bid=0.00 ask=1108.80 bid_depth_qty=0 ask_depth_qty=2111 imbalance=-1.000 ema_fast=1108.80 ema_slow=1108.80 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1059
[worker] symbol=ICICIBANK state=Monitoring last=1601.30 bid=1601.30 ask=0.00 bid_depth_qty=7004 ask_depth_qty=0 imbalance=1.000 ema_fast=1601.30 ema_slow=1601.30 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1059
[worker] symbol=RELIANCE state=Monitoring last=1363.00 bid=0.00 ask=1363.00 bid_depth_qty=0 ask_depth_qty=5121 imbalance=-1.000 ema_fast=1363.00 ema_slow=1363.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1059
[worker] symbol=HDFCBANK state=Monitoring last=808.15 bid=0.00 ask=808.15 bid_depth_qty=0 ask_depth_qty=4942 imbalance=-1.000 ema_fast=808.15 ema_slow=808.15 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1059
[worker] symbol=LT state=Monitoring last=4159.00 bid=0.00 ask=4159.00 bid_depth_qty=0 ask_depth_qty=835 imbalance=-1.000 ema_fast=4159.00 ema_slow=4159.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=987
[worker] symbol=NTPC state=Monitoring last=320.40 bid=0.00 ask=320.40 bid_depth_qty=0 ask_depth_qty=2169 imbalance=-1.000 ema_fast=320.40 ema_slow=320.40 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=987
[worker] symbol=HINDUNILVR state=Monitoring last=1993.00 bid=1993.00 ask=0.00 bid_depth_qty=45 ask_depth_qty=0 imbalance=1.000 ema_fast=1993.00 ema_slow=1993.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=987
[worker] symbol=AXISBANK state=Monitoring last=1360.00 bid=0.00 ask=1360.00 bid_depth_qty=0 ask_depth_qty=2255 imbalance=-1.000 ema_fast=1360.00 ema_slow=1360.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=987
[worker] symbol=BAJFINANCE state=Monitoring last=1093.85 bid=0.00 ask=1093.85 bid_depth_qty=0 ask_depth_qty=1564 imbalance=-1.000 ema_fast=1093.85 ema_slow=1093.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=987
[worker] symbol=KOTAKBANK state=Monitoring last=494.85 bid=0.00 ask=494.85 bid_depth_qty=0 ask_depth_qty=5881 imbalance=-1.000 ema_fast=494.85 ema_slow=494.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=987
[worker] symbol=M&M state=Monitoring last=3359.10 bid=0.00 ask=3359.10 bid_depth_qty=0 ask_depth_qty=1139 imbalance=-1.000 ema_fast=3359.10 ema_slow=3359.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=987
[worker] symbol=MARUTI state=Monitoring last=12632.00 bid=0.00 ask=12632.00 bid_depth_qty=0 ask_depth_qty=18 imbalance=-1.000 ema_fast=12632.00 ema_slow=12632.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=990
[worker] symbol=SUNPHARMA state=Monitoring last=1871.50 bid=0.00 ask=1871.50 bid_depth_qty=0 ask_depth_qty=38 imbalance=-1.000 ema_fast=1871.50 ema_slow=1871.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=990
[worker] symbol=TITAN state=Monitoring last=4648.10 bid=4648.10 ask=0.00 bid_depth_qty=21 ask_depth_qty=0 imbalance=1.000 ema_fast=4648.10 ema_slow=4648.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=991
[worker] symbol=ONGC state=Monitoring last=225.00 bid=0.00 ask=225.00 bid_depth_qty=0 ask_depth_qty=20951 imbalance=-1.000 ema_fast=225.00 ema_slow=225.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=991
[worker] symbol=ULTRACEMCO state=Monitoring last=12428.00 bid=0.00 ask=12428.00 bid_depth_qty=0 ask_depth_qty=274 imbalance=-1.000 ema_fast=12428.00 ema_slow=12428.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=992
[worker] symbol=ICICIBANK state=Monitoring last=1601.30 bid=1601.30 ask=0.00 bid_depth_qty=7004 ask_depth_qty=0 imbalance=1.000 ema_fast=1601.30 ema_slow=1601.30 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1002
[worker] symbol=HDFCBANK state=Monitoring last=808.15 bid=0.00 ask=808.15 bid_depth_qty=0 ask_depth_qty=4942 imbalance=-1.000 ema_fast=808.15 ema_slow=808.15 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1002
[worker] symbol=RELIANCE state=Monitoring last=1363.00 bid=0.00 ask=1363.00 bid_depth_qty=0 ask_depth_qty=5121 imbalance=-1.000 ema_fast=1363.00 ema_slow=1363.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1002
[worker] symbol=TCS state=Monitoring last=2379.50 bid=2379.50 ask=0.00 bid_depth_qty=666 ask_depth_qty=0 imbalance=1.000 ema_fast=2379.50 ema_slow=2379.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1003
[worker] symbol=INFY state=Monitoring last=1108.80 bid=0.00 ask=1108.80 bid_depth_qty=0 ask_depth_qty=2111 imbalance=-1.000 ema_fast=1108.80 ema_slow=1108.80 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1003
[worker] symbol=SBIN state=Monitoring last=1135.00 bid=0.00 ask=1135.00 bid_depth_qty=0 ask_depth_qty=2961 imbalance=-1.000 ema_fast=1135.00 ema_slow=1135.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1003
[worker] symbol=ITC state=Monitoring last=296.60 bid=296.60 ask=0.00 bid_depth_qty=1707 ask_depth_qty=0 imbalance=1.000 ema_fast=296.60 ema_slow=296.60 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1003
[worker] symbol=BHARTIARTL state=Monitoring last=2132.00 bid=0.00 ask=2132.00 bid_depth_qty=0 ask_depth_qty=2097 imbalance=-1.000 ema_fast=2132.00 ema_slow=2132.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1003
[worker] symbol=SUNPHARMA state=Monitoring last=1871.50 bid=0.00 ask=1871.50 bid_depth_qty=0 ask_depth_qty=38 imbalance=-1.000 ema_fast=1871.50 ema_slow=1871.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=936
[worker] symbol=MARUTI state=Monitoring last=12632.00 bid=0.00 ask=12632.00 bid_depth_qty=0 ask_depth_qty=18 imbalance=-1.000 ema_fast=12632.00 ema_slow=12632.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=936
[worker] symbol=M&M state=Monitoring last=3359.10 bid=0.00 ask=3359.10 bid_depth_qty=0 ask_depth_qty=1139 imbalance=-1.000 ema_fast=3359.10 ema_slow=3359.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=936
[worker] symbol=KOTAKBANK state=Monitoring last=494.85 bid=0.00 ask=494.85 bid_depth_qty=0 ask_depth_qty=5881 imbalance=-1.000 ema_fast=494.85 ema_slow=494.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=936
[worker] symbol=BAJFINANCE state=Monitoring last=1093.85 bid=0.00 ask=1093.85 bid_depth_qty=0 ask_depth_qty=1564 imbalance=-1.000 ema_fast=1093.85 ema_slow=1093.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=936
[worker] symbol=AXISBANK state=Monitoring last=1360.00 bid=0.00 ask=1360.00 bid_depth_qty=0 ask_depth_qty=2255 imbalance=-1.000 ema_fast=1360.00 ema_slow=1360.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=936
[worker] symbol=HINDUNILVR state=Monitoring last=1993.00 bid=1993.00 ask=0.00 bid_depth_qty=45 ask_depth_qty=0 imbalance=1.000 ema_fast=1993.00 ema_slow=1993.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=936
[worker] symbol=NTPC state=Monitoring last=320.40 bid=0.00 ask=320.40 bid_depth_qty=0 ask_depth_qty=2169 imbalance=-1.000 ema_fast=320.40 ema_slow=320.40 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=936
[worker] symbol=LT state=Monitoring last=4159.00 bid=0.00 ask=4159.00 bid_depth_qty=0 ask_depth_qty=835 imbalance=-1.000 ema_fast=4159.00 ema_slow=4159.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=936
[worker] symbol=TITAN state=Monitoring last=4648.10 bid=4648.10 ask=0.00 bid_depth_qty=21 ask_depth_qty=0 imbalance=1.000 ema_fast=4648.10 ema_slow=4648.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=936
[worker] symbol=ONGC state=Monitoring last=225.00 bid=0.00 ask=225.00 bid_depth_qty=0 ask_depth_qty=20951 imbalance=-1.000 ema_fast=225.00 ema_slow=225.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=936
[worker] symbol=ULTRACEMCO state=Monitoring last=12428.00 bid=0.00 ask=12428.00 bid_depth_qty=0 ask_depth_qty=274 imbalance=-1.000 ema_fast=12428.00 ema_slow=12428.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=952
[worker] symbol=BHARTIARTL state=Monitoring last=2132.00 bid=0.00 ask=2132.00 bid_depth_qty=0 ask_depth_qty=2097 imbalance=-1.000 ema_fast=2132.00 ema_slow=2132.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=953
[worker] symbol=SBIN state=Monitoring last=1135.00 bid=0.00 ask=1135.00 bid_depth_qty=0 ask_depth_qty=2961 imbalance=-1.000 ema_fast=1135.00 ema_slow=1135.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=953
[worker] symbol=ITC state=Monitoring last=296.60 bid=296.60 ask=0.00 bid_depth_qty=1707 ask_depth_qty=0 imbalance=1.000 ema_fast=296.60 ema_slow=296.60 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=953
[worker] symbol=INFY state=Monitoring last=1108.80 bid=0.00 ask=1108.80 bid_depth_qty=0 ask_depth_qty=2111 imbalance=-1.000 ema_fast=1108.80 ema_slow=1108.80 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=953
[worker] symbol=TCS state=Monitoring last=2379.50 bid=2379.50 ask=0.00 bid_depth_qty=666 ask_depth_qty=0 imbalance=1.000 ema_fast=2379.50 ema_slow=2379.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=953
[worker] symbol=RELIANCE state=Monitoring last=1363.00 bid=0.00 ask=1363.00 bid_depth_qty=0 ask_depth_qty=5121 imbalance=-1.000 ema_fast=1363.00 ema_slow=1363.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=953
[worker] symbol=HDFCBANK state=Monitoring last=808.15 bid=0.00 ask=808.15 bid_depth_qty=0 ask_depth_qty=4942 imbalance=-1.000 ema_fast=808.15 ema_slow=808.15 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=953
[worker] symbol=ICICIBANK state=Monitoring last=1601.30 bid=1601.30 ask=0.00 bid_depth_qty=7004 ask_depth_qty=0 imbalance=1.000 ema_fast=1601.30 ema_slow=1601.30 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=953
[worker] symbol=TITAN state=Monitoring last=4648.10 bid=4648.10 ask=0.00 bid_depth_qty=21 ask_depth_qty=0 imbalance=1.000 ema_fast=4648.10 ema_slow=4648.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=888
[worker] symbol=LT state=Monitoring last=4159.00 bid=0.00 ask=4159.00 bid_depth_qty=0 ask_depth_qty=835 imbalance=-1.000 ema_fast=4159.00 ema_slow=4159.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=889
[worker] symbol=ONGC state=Monitoring last=225.00 bid=0.00 ask=225.00 bid_depth_qty=0 ask_depth_qty=20951 imbalance=-1.000 ema_fast=225.00 ema_slow=225.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=888
[worker] symbol=NTPC state=Monitoring last=320.40 bid=0.00 ask=320.40 bid_depth_qty=0 ask_depth_qty=2169 imbalance=-1.000 ema_fast=320.40 ema_slow=320.40 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=889
[worker] symbol=HINDUNILVR state=Monitoring last=1993.00 bid=1993.00 ask=0.00 bid_depth_qty=45 ask_depth_qty=0 imbalance=1.000 ema_fast=1993.00 ema_slow=1993.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=889
[worker] symbol=AXISBANK state=Monitoring last=1360.00 bid=0.00 ask=1360.00 bid_depth_qty=0 ask_depth_qty=2255 imbalance=-1.000 ema_fast=1360.00 ema_slow=1360.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=889
[worker] symbol=BAJFINANCE state=Monitoring last=1093.85 bid=0.00 ask=1093.85 bid_depth_qty=0 ask_depth_qty=1564 imbalance=-1.000 ema_fast=1093.85 ema_slow=1093.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=889
[worker] symbol=KOTAKBANK state=Monitoring last=494.85 bid=0.00 ask=494.85 bid_depth_qty=0 ask_depth_qty=5881 imbalance=-1.000 ema_fast=494.85 ema_slow=494.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=889
[worker] symbol=M&M state=Monitoring last=3359.10 bid=0.00 ask=3359.10 bid_depth_qty=0 ask_depth_qty=1139 imbalance=-1.000 ema_fast=3359.10 ema_slow=3359.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=889
[worker] symbol=MARUTI state=Monitoring last=12632.00 bid=0.00 ask=12632.00 bid_depth_qty=0 ask_depth_qty=18 imbalance=-1.000 ema_fast=12632.00 ema_slow=12632.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=889
[worker] symbol=SUNPHARMA state=Monitoring last=1871.50 bid=0.00 ask=1871.50 bid_depth_qty=0 ask_depth_qty=38 imbalance=-1.000 ema_fast=1871.50 ema_slow=1871.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=889
[worker] symbol=RELIANCE state=Monitoring last=1363.00 bid=0.00 ask=1363.00 bid_depth_qty=0 ask_depth_qty=5121 imbalance=-1.000 ema_fast=1363.00 ema_slow=1363.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=904
[worker] symbol=TCS state=Monitoring last=2379.50 bid=2379.50 ask=0.00 bid_depth_qty=666 ask_depth_qty=0 imbalance=1.000 ema_fast=2379.50 ema_slow=2379.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=904
[worker] symbol=HDFCBANK state=Monitoring last=808.15 bid=0.00 ask=808.15 bid_depth_qty=0 ask_depth_qty=4942 imbalance=-1.000 ema_fast=808.15 ema_slow=808.15 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=904
[worker] symbol=ICICIBANK state=Monitoring last=1601.30 bid=1601.30 ask=0.00 bid_depth_qty=7004 ask_depth_qty=0 imbalance=1.000 ema_fast=1601.30 ema_slow=1601.30 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=904
[worker] symbol=INFY state=Monitoring last=1108.80 bid=0.00 ask=1108.80 bid_depth_qty=0 ask_depth_qty=2111 imbalance=-1.000 ema_fast=1108.80 ema_slow=1108.80 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=905
[worker] symbol=ITC state=Monitoring last=296.60 bid=296.60 ask=0.00 bid_depth_qty=1707 ask_depth_qty=0 imbalance=1.000 ema_fast=296.60 ema_slow=296.60 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=905
[worker] symbol=SBIN state=Monitoring last=1135.00 bid=0.00 ask=1135.00 bid_depth_qty=0 ask_depth_qty=2961 imbalance=-1.000 ema_fast=1135.00 ema_slow=1135.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=905
[worker] symbol=BHARTIARTL state=Monitoring last=2132.00 bid=0.00 ask=2132.00 bid_depth_qty=0 ask_depth_qty=2097 imbalance=-1.000 ema_fast=2132.00 ema_slow=2132.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=905
[worker] symbol=ULTRACEMCO state=Monitoring last=12428.00 bid=0.00 ask=12428.00 bid_depth_qty=0 ask_depth_qty=274 imbalance=-1.000 ema_fast=12428.00 ema_slow=12428.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=905
[worker] symbol=MARUTI state=Monitoring last=12632.00 bid=0.00 ask=12632.00 bid_depth_qty=0 ask_depth_qty=18 imbalance=-1.000 ema_fast=12632.00 ema_slow=12632.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=832
[worker] symbol=SUNPHARMA state=Monitoring last=1871.50 bid=0.00 ask=1871.50 bid_depth_qty=0 ask_depth_qty=38 imbalance=-1.000 ema_fast=1871.50 ema_slow=1871.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=832
[worker] symbol=M&M state=Monitoring last=3359.10 bid=0.00 ask=3359.10 bid_depth_qty=0 ask_depth_qty=1139 imbalance=-1.000 ema_fast=3359.10 ema_slow=3359.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=832
[worker] symbol=KOTAKBANK state=Monitoring last=494.85 bid=0.00 ask=494.85 bid_depth_qty=0 ask_depth_qty=5881 imbalance=-1.000 ema_fast=494.85 ema_slow=494.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=832
[worker] symbol=BAJFINANCE state=Monitoring last=1093.85 bid=0.00 ask=1093.85 bid_depth_qty=0 ask_depth_qty=1564 imbalance=-1.000 ema_fast=1093.85 ema_slow=1093.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=832
[worker] symbol=HINDUNILVR state=Monitoring last=1993.00 bid=1993.00 ask=0.00 bid_depth_qty=45 ask_depth_qty=0 imbalance=1.000 ema_fast=1993.00 ema_slow=1993.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=833
[worker] symbol=AXISBANK state=Monitoring last=1360.00 bid=0.00 ask=1360.00 bid_depth_qty=0 ask_depth_qty=2255 imbalance=-1.000 ema_fast=1360.00 ema_slow=1360.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=832
[worker] symbol=ONGC state=Monitoring last=225.00 bid=0.00 ask=225.00 bid_depth_qty=0 ask_depth_qty=20951 imbalance=-1.000 ema_fast=225.00 ema_slow=225.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=833
[worker] symbol=NTPC state=Monitoring last=320.40 bid=0.00 ask=320.40 bid_depth_qty=0 ask_depth_qty=2169 imbalance=-1.000 ema_fast=320.40 ema_slow=320.40 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=833
[worker] symbol=LT state=Monitoring last=4159.00 bid=0.00 ask=4159.00 bid_depth_qty=0 ask_depth_qty=835 imbalance=-1.000 ema_fast=4159.00 ema_slow=4159.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=833
[worker] symbol=TITAN state=Monitoring last=4648.10 bid=4648.10 ask=0.00 bid_depth_qty=21 ask_depth_qty=0 imbalance=1.000 ema_fast=4648.10 ema_slow=4648.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=833
[worker] symbol=BHARTIARTL state=Monitoring last=2132.00 bid=0.00 ask=2132.00 bid_depth_qty=0 ask_depth_qty=2097 imbalance=-1.000 ema_fast=2132.00 ema_slow=2132.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=848
[worker] symbol=ULTRACEMCO state=Monitoring last=12428.00 bid=0.00 ask=12428.00 bid_depth_qty=0 ask_depth_qty=274 imbalance=-1.000 ema_fast=12428.00 ema_slow=12428.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=848
[worker] symbol=SBIN state=Monitoring last=1135.00 bid=0.00 ask=1135.00 bid_depth_qty=0 ask_depth_qty=2961 imbalance=-1.000 ema_fast=1135.00 ema_slow=1135.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=848
[worker] symbol=ITC state=Monitoring last=296.60 bid=296.60 ask=0.00 bid_depth_qty=1707 ask_depth_qty=0 imbalance=1.000 ema_fast=296.60 ema_slow=296.60 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=848
[worker] symbol=INFY state=Monitoring last=1108.80 bid=0.00 ask=1108.80 bid_depth_qty=0 ask_depth_qty=2111 imbalance=-1.000 ema_fast=1108.80 ema_slow=1108.80 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=848
[worker] symbol=ICICIBANK state=Monitoring last=1601.30 bid=1601.30 ask=0.00 bid_depth_qty=7004 ask_depth_qty=0 imbalance=1.000 ema_fast=1601.30 ema_slow=1601.30 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=848
[worker] symbol=TCS state=Monitoring last=2379.50 bid=2379.50 ask=0.00 bid_depth_qty=666 ask_depth_qty=0 imbalance=1.000 ema_fast=2379.50 ema_slow=2379.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=848
[worker] symbol=HDFCBANK state=Monitoring last=808.15 bid=0.00 ask=808.15 bid_depth_qty=0 ask_depth_qty=4942 imbalance=-1.000 ema_fast=808.15 ema_slow=808.15 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=848
[worker] symbol=RELIANCE state=Monitoring last=1363.00 bid=0.00 ask=1363.00 bid_depth_qty=0 ask_depth_qty=5121 imbalance=-1.000 ema_fast=1363.00 ema_slow=1363.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=848
[worker] symbol=TITAN state=Monitoring last=4648.10 bid=4648.10 ask=0.00 bid_depth_qty=21 ask_depth_qty=0 imbalance=1.000 ema_fast=4648.10 ema_slow=4648.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=776
[worker] symbol=NTPC state=Monitoring last=320.40 bid=0.00 ask=320.40 bid_depth_qty=0 ask_depth_qty=2169 imbalance=-1.000 ema_fast=320.40 ema_slow=320.40 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=776
[worker] symbol=LT state=Monitoring last=4159.00 bid=0.00 ask=4159.00 bid_depth_qty=0 ask_depth_qty=835 imbalance=-1.000 ema_fast=4159.00 ema_slow=4159.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=776
[worker] symbol=ONGC state=Monitoring last=225.00 bid=0.00 ask=225.00 bid_depth_qty=0 ask_depth_qty=20951 imbalance=-1.000 ema_fast=225.00 ema_slow=225.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=776
[worker] symbol=AXISBANK state=Monitoring last=1360.00 bid=0.00 ask=1360.00 bid_depth_qty=0 ask_depth_qty=2255 imbalance=-1.000 ema_fast=1360.00 ema_slow=1360.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=776
[worker] symbol=BAJFINANCE state=Monitoring last=1093.85 bid=0.00 ask=1093.85 bid_depth_qty=0 ask_depth_qty=1564 imbalance=-1.000 ema_fast=1093.85 ema_slow=1093.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=776
[worker] symbol=HINDUNILVR state=Monitoring last=1993.00 bid=1993.00 ask=0.00 bid_depth_qty=45 ask_depth_qty=0 imbalance=1.000 ema_fast=1993.00 ema_slow=1993.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=776
[worker] symbol=M&M state=Monitoring last=3359.10 bid=0.00 ask=3359.10 bid_depth_qty=0 ask_depth_qty=1139 imbalance=-1.000 ema_fast=3359.10 ema_slow=3359.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=776
[worker] symbol=KOTAKBANK state=Monitoring last=494.85 bid=0.00 ask=494.85 bid_depth_qty=0 ask_depth_qty=5881 imbalance=-1.000 ema_fast=494.85 ema_slow=494.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=776
[worker] symbol=SUNPHARMA state=Monitoring last=1871.50 bid=0.00 ask=1871.50 bid_depth_qty=0 ask_depth_qty=38 imbalance=-1.000 ema_fast=1871.50 ema_slow=1871.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=776
[worker] symbol=MARUTI state=Monitoring last=12632.00 bid=0.00 ask=12632.00 bid_depth_qty=0 ask_depth_qty=18 imbalance=-1.000 ema_fast=12632.00 ema_slow=12632.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=776
[worker] symbol=INFY state=Monitoring last=1108.80 bid=0.00 ask=1108.80 bid_depth_qty=0 ask_depth_qty=2111 imbalance=-1.000 ema_fast=1108.80 ema_slow=1108.80 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=792
[worker] symbol=ICICIBANK state=Monitoring last=1601.30 bid=1601.30 ask=0.00 bid_depth_qty=7004 ask_depth_qty=0 imbalance=1.000 ema_fast=1601.30 ema_slow=1601.30 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=792
[worker] symbol=ITC state=Monitoring last=296.60 bid=296.60 ask=0.00 bid_depth_qty=1707 ask_depth_qty=0 imbalance=1.000 ema_fast=296.60 ema_slow=296.60 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=792
[worker] symbol=SBIN state=Monitoring last=1135.00 bid=0.00 ask=1135.00 bid_depth_qty=0 ask_depth_qty=2961 imbalance=-1.000 ema_fast=1135.00 ema_slow=1135.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=792
[worker] symbol=ULTRACEMCO state=Monitoring last=12428.00 bid=0.00 ask=12428.00 bid_depth_qty=0 ask_depth_qty=274 imbalance=-1.000 ema_fast=12428.00 ema_slow=12428.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=792
[worker] symbol=BHARTIARTL state=Monitoring last=2132.00 bid=0.00 ask=2132.00 bid_depth_qty=0 ask_depth_qty=2097 imbalance=-1.000 ema_fast=2132.00 ema_slow=2132.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=792
[worker] symbol=HDFCBANK state=Monitoring last=808.15 bid=0.00 ask=808.15 bid_depth_qty=0 ask_depth_qty=4942 imbalance=-1.000 ema_fast=808.15 ema_slow=808.15 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=795
[worker] symbol=TCS state=Monitoring last=2379.50 bid=2379.50 ask=0.00 bid_depth_qty=666 ask_depth_qty=0 imbalance=1.000 ema_fast=2379.50 ema_slow=2379.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=795
[worker] symbol=RELIANCE state=Monitoring last=1363.00 bid=0.00 ask=1363.00 bid_depth_qty=0 ask_depth_qty=5121 imbalance=-1.000 ema_fast=1363.00 ema_slow=1363.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=795
[worker] symbol=NTPC state=Monitoring last=320.40 bid=0.00 ask=320.40 bid_depth_qty=0 ask_depth_qty=2169 imbalance=-1.000 ema_fast=320.40 ema_slow=320.40 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=725
[worker] symbol=TITAN state=Monitoring last=4648.10 bid=4648.10 ask=0.00 bid_depth_qty=21 ask_depth_qty=0 imbalance=1.000 ema_fast=4648.10 ema_slow=4648.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=725
[worker] symbol=LT state=Monitoring last=4159.00 bid=0.00 ask=4159.00 bid_depth_qty=0 ask_depth_qty=835 imbalance=-1.000 ema_fast=4159.00 ema_slow=4159.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=725
[worker] symbol=SUNPHARMA state=Monitoring last=1871.50 bid=0.00 ask=1871.50 bid_depth_qty=0 ask_depth_qty=38 imbalance=-1.000 ema_fast=1871.50 ema_slow=1871.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=726
[worker] symbol=MARUTI state=Monitoring last=12632.00 bid=0.00 ask=12632.00 bid_depth_qty=0 ask_depth_qty=18 imbalance=-1.000 ema_fast=12632.00 ema_slow=12632.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=726
[worker] symbol=KOTAKBANK state=Monitoring last=494.85 bid=0.00 ask=494.85 bid_depth_qty=0 ask_depth_qty=5881 imbalance=-1.000 ema_fast=494.85 ema_slow=494.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=726
[worker] symbol=HINDUNILVR state=Monitoring last=1993.00 bid=1993.00 ask=0.00 bid_depth_qty=45 ask_depth_qty=0 imbalance=1.000 ema_fast=1993.00 ema_slow=1993.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=726
[worker] symbol=M&M state=Monitoring last=3359.10 bid=0.00 ask=3359.10 bid_depth_qty=0 ask_depth_qty=1139 imbalance=-1.000 ema_fast=3359.10 ema_slow=3359.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=726
[worker] symbol=ONGC state=Monitoring last=225.00 bid=0.00 ask=225.00 bid_depth_qty=0 ask_depth_qty=20951 imbalance=-1.000 ema_fast=225.00 ema_slow=225.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=726
[worker] symbol=AXISBANK state=Monitoring last=1360.00 bid=0.00 ask=1360.00 bid_depth_qty=0 ask_depth_qty=2255 imbalance=-1.000 ema_fast=1360.00 ema_slow=1360.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=726
[worker] symbol=BAJFINANCE state=Monitoring last=1093.85 bid=0.00 ask=1093.85 bid_depth_qty=0 ask_depth_qty=1564 imbalance=-1.000 ema_fast=1093.85 ema_slow=1093.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=726
[worker] symbol=TCS state=Monitoring last=2379.50 bid=2379.50 ask=0.00 bid_depth_qty=666 ask_depth_qty=0 imbalance=1.000 ema_fast=2379.50 ema_slow=2379.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=741
[worker] symbol=RELIANCE state=Monitoring last=1363.00 bid=0.00 ask=1363.00 bid_depth_qty=0 ask_depth_qty=5121 imbalance=-1.000 ema_fast=1363.00 ema_slow=1363.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=741
[worker] symbol=BHARTIARTL state=Monitoring last=2132.00 bid=0.00 ask=2132.00 bid_depth_qty=0 ask_depth_qty=2097 imbalance=-1.000 ema_fast=2132.00 ema_slow=2132.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=741
[worker] symbol=HDFCBANK state=Monitoring last=808.15 bid=0.00 ask=808.15 bid_depth_qty=0 ask_depth_qty=4942 imbalance=-1.000 ema_fast=808.15 ema_slow=808.15 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=741
[worker] symbol=ULTRACEMCO state=Monitoring last=12428.00 bid=0.00 ask=12428.00 bid_depth_qty=0 ask_depth_qty=274 imbalance=-1.000 ema_fast=12428.00 ema_slow=12428.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=741
[worker] symbol=ITC state=Monitoring last=296.60 bid=296.60 ask=0.00 bid_depth_qty=1707 ask_depth_qty=0 imbalance=1.000 ema_fast=296.60 ema_slow=296.60 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=741
[worker] symbol=SBIN state=Monitoring last=1135.00 bid=0.00 ask=1135.00 bid_depth_qty=0 ask_depth_qty=2961 imbalance=-1.000 ema_fast=1135.00 ema_slow=1135.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=741
[worker] symbol=INFY state=Monitoring last=1108.80 bid=0.00 ask=1108.80 bid_depth_qty=0 ask_depth_qty=2111 imbalance=-1.000 ema_fast=1108.80 ema_slow=1108.80 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=742
[worker] symbol=ICICIBANK state=Monitoring last=1601.30 bid=1601.30 ask=0.00 bid_depth_qty=7004 ask_depth_qty=0 imbalance=1.000 ema_fast=1601.30 ema_slow=1601.30 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=742
[worker] symbol=ONGC state=Monitoring last=225.00 bid=0.00 ask=225.00 bid_depth_qty=0 ask_depth_qty=20951 imbalance=-1.000 ema_fast=225.00 ema_slow=225.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=673
[worker] symbol=AXISBANK state=Monitoring last=1360.00 bid=0.00 ask=1360.00 bid_depth_qty=0 ask_depth_qty=2255 imbalance=-1.000 ema_fast=1360.00 ema_slow=1360.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=673
[worker] symbol=BAJFINANCE state=Monitoring last=1093.85 bid=0.00 ask=1093.85 bid_depth_qty=0 ask_depth_qty=1564 imbalance=-1.000 ema_fast=1093.85 ema_slow=1093.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=673
[worker] symbol=M&M state=Monitoring last=3359.10 bid=0.00 ask=3359.10 bid_depth_qty=0 ask_depth_qty=1139 imbalance=-1.000 ema_fast=3359.10 ema_slow=3359.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=673
[worker] symbol=KOTAKBANK state=Monitoring last=494.85 bid=0.00 ask=494.85 bid_depth_qty=0 ask_depth_qty=5881 imbalance=-1.000 ema_fast=494.85 ema_slow=494.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=673
[worker] symbol=HINDUNILVR state=Monitoring last=1993.00 bid=1993.00 ask=0.00 bid_depth_qty=45 ask_depth_qty=0 imbalance=1.000 ema_fast=1993.00 ema_slow=1993.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=673
[worker] symbol=MARUTI state=Monitoring last=12632.00 bid=0.00 ask=12632.00 bid_depth_qty=0 ask_depth_qty=18 imbalance=-1.000 ema_fast=12632.00 ema_slow=12632.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=673
[worker] symbol=SUNPHARMA state=Monitoring last=1871.50 bid=0.00 ask=1871.50 bid_depth_qty=0 ask_depth_qty=38 imbalance=-1.000 ema_fast=1871.50 ema_slow=1871.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=674
[worker] symbol=TITAN state=Monitoring last=4648.10 bid=4648.10 ask=0.00 bid_depth_qty=21 ask_depth_qty=0 imbalance=1.000 ema_fast=4648.10 ema_slow=4648.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=674
[worker] symbol=LT state=Monitoring last=4159.00 bid=0.00 ask=4159.00 bid_depth_qty=0 ask_depth_qty=835 imbalance=-1.000 ema_fast=4159.00 ema_slow=4159.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=674
[worker] symbol=NTPC state=Monitoring last=320.40 bid=0.00 ask=320.40 bid_depth_qty=0 ask_depth_qty=2169 imbalance=-1.000 ema_fast=320.40 ema_slow=320.40 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=674
[worker] symbol=INFY state=Monitoring last=1108.80 bid=0.00 ask=1108.80 bid_depth_qty=0 ask_depth_qty=2111 imbalance=-1.000 ema_fast=1108.80 ema_slow=1108.80 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=689
[worker] symbol=ICICIBANK state=Monitoring last=1601.30 bid=1601.30 ask=0.00 bid_depth_qty=7004 ask_depth_qty=0 imbalance=1.000 ema_fast=1601.30 ema_slow=1601.30 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=689
[worker] symbol=SBIN state=Monitoring last=1135.00 bid=0.00 ask=1135.00 bid_depth_qty=0 ask_depth_qty=2961 imbalance=-1.000 ema_fast=1135.00 ema_slow=1135.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=689
[worker] symbol=ITC state=Monitoring last=296.60 bid=296.60 ask=0.00 bid_depth_qty=1707 ask_depth_qty=0 imbalance=1.000 ema_fast=296.60 ema_slow=296.60 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=689
[worker] symbol=ULTRACEMCO state=Monitoring last=12428.00 bid=0.00 ask=12428.00 bid_depth_qty=0 ask_depth_qty=274 imbalance=-1.000 ema_fast=12428.00 ema_slow=12428.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=689
[worker] symbol=HDFCBANK state=Monitoring last=808.15 bid=0.00 ask=808.15 bid_depth_qty=0 ask_depth_qty=4942 imbalance=-1.000 ema_fast=808.15 ema_slow=808.15 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=689
[worker] symbol=RELIANCE state=Monitoring last=1363.00 bid=0.00 ask=1363.00 bid_depth_qty=0 ask_depth_qty=5121 imbalance=-1.000 ema_fast=1363.00 ema_slow=1363.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=689
[worker] symbol=BHARTIARTL state=Monitoring last=2132.00 bid=0.00 ask=2132.00 bid_depth_qty=0 ask_depth_qty=2097 imbalance=-1.000 ema_fast=2132.00 ema_slow=2132.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=689
[worker] symbol=TCS state=Monitoring last=2379.50 bid=2379.50 ask=0.00 bid_depth_qty=666 ask_depth_qty=0 imbalance=1.000 ema_fast=2379.50 ema_slow=2379.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=689
Shutdown requested; stopping workers
[worker] symbol=LT state=Monitoring last=4159.00 bid=0.00 ask=4159.00 bid_depth_qty=0 ask_depth_qty=835 imbalance=-1.000 ema_fast=4159.00 ema_slow=4159.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=625
[worker] symbol=NTPC state=Monitoring last=320.40 bid=0.00 ask=320.40 bid_depth_qty=0 ask_depth_qty=2169 imbalance=-1.000 ema_fast=320.40 ema_slow=320.40 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=625
[worker] symbol=TITAN state=Monitoring last=4648.10 bid=4648.10 ask=0.00 bid_depth_qty=21 ask_depth_qty=0 imbalance=1.000 ema_fast=4648.10 ema_slow=4648.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=625
[worker] symbol=MARUTI state=Monitoring last=12632.00 bid=0.00 ask=12632.00 bid_depth_qty=0 ask_depth_qty=18 imbalance=-1.000 ema_fast=12632.00 ema_slow=12632.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=625
[worker] symbol=HINDUNILVR state=Monitoring last=1993.00 bid=1993.00 ask=0.00 bid_depth_qty=45 ask_depth_qty=0 imbalance=1.000 ema_fast=1993.00 ema_slow=1993.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=625
[worker] symbol=M&M state=Monitoring last=3359.10 bid=0.00 ask=3359.10 bid_depth_qty=0 ask_depth_qty=1139 imbalance=-1.000 ema_fast=3359.10 ema_slow=3359.10 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=626
[worker] symbol=SUNPHARMA state=Monitoring last=1871.50 bid=0.00 ask=1871.50 bid_depth_qty=0 ask_depth_qty=38 imbalance=-1.000 ema_fast=1871.50 ema_slow=1871.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=625
[worker] symbol=KOTAKBANK state=Monitoring last=494.85 bid=0.00 ask=494.85 bid_depth_qty=0 ask_depth_qty=5881 imbalance=-1.000 ema_fast=494.85 ema_slow=494.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=626
[worker] symbol=BAJFINANCE state=Monitoring last=1093.85 bid=0.00 ask=1093.85 bid_depth_qty=0 ask_depth_qty=1564 imbalance=-1.000 ema_fast=1093.85 ema_slow=1093.85 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=626
[worker] symbol=ONGC state=Monitoring last=225.00 bid=0.00 ask=225.00 bid_depth_qty=0 ask_depth_qty=20951 imbalance=-1.000 ema_fast=225.00 ema_slow=225.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=626
[worker] symbol=AXISBANK state=Monitoring last=1360.00 bid=0.00 ask=1360.00 bid_depth_qty=0 ask_depth_qty=2255 imbalance=-1.000 ema_fast=1360.00 ema_slow=1360.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=626
[worker] symbol=TCS state=Monitoring last=2379.50 bid=2379.50 ask=0.00 bid_depth_qty=666 ask_depth_qty=0 imbalance=1.000 ema_fast=2379.50 ema_slow=2379.50 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=641
[worker] symbol=BHARTIARTL state=Monitoring last=2132.00 bid=0.00 ask=2132.00 bid_depth_qty=0 ask_depth_qty=2097 imbalance=-1.000 ema_fast=2132.00 ema_slow=2132.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=641
[worker] symbol=RELIANCE state=Monitoring last=1363.00 bid=0.00 ask=1363.00 bid_depth_qty=0 ask_depth_qty=5121 imbalance=-1.000 ema_fast=1363.00 ema_slow=1363.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=641
[worker] symbol=ULTRACEMCO state=Monitoring last=12428.00 bid=0.00 ask=12428.00 bid_depth_qty=0 ask_depth_qty=274 imbalance=-1.000 ema_fast=12428.00 ema_slow=12428.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=641
[worker] symbol=ITC state=Monitoring last=296.60 bid=296.60 ask=0.00 bid_depth_qty=1707 ask_depth_qty=0 imbalance=1.000 ema_fast=296.60 ema_slow=296.60 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=641
[worker] symbol=HDFCBANK state=Monitoring last=808.15 bid=0.00 ask=808.15 bid_depth_qty=0 ask_depth_qty=4942 imbalance=-1.000 ema_fast=808.15 ema_slow=808.15 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=641
[worker] symbol=ICICIBANK state=Monitoring last=1601.30 bid=1601.30 ask=0.00 bid_depth_qty=7004 ask_depth_qty=0 imbalance=1.000 ema_fast=1601.30 ema_slow=1601.30 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=642
[worker] symbol=SBIN state=Monitoring last=1135.00 bid=0.00 ask=1135.00 bid_depth_qty=0 ask_depth_qty=2961 imbalance=-1.000 ema_fast=1135.00 ema_slow=1135.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=641
[worker] symbol=INFY state=Monitoring last=1108.80 bid=0.00 ask=1108.80 bid_depth_qty=0 ask_depth_qty=2111 imbalance=-1.000 ema_fast=1108.80 ema_slow=1108.80 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=642
PS C:\Users\Abhishek Tyagi\Desktop\ZeroTick-Systems\ZeroTick-Systems-1> 


19th November

PS C:\Users\Abhishek Tyagi\Desktop\ZeroTick-Systems\ZeroTick-Systems-1> .\build\Release\zerotick.exe --login
Starting Kite login in your browser...
Waiting for the Kite login callback on localhost...
Kite login callback received; exchanging token.
Kite access token obtained and saved in local .env
[kite] quote_batch=received instruments=20
Started 1 of 20 stock worker threads; 19 disabled; active_symbol=RELIANCE
[worker] symbol=RELIANCE state=Monitoring last=1358.00 bid=1357.80 ask=1358.00 bid_depth_qty=2078 ask_depth_qty=5642 imbalance=-0.462 ema_fast=1358.00 ema_slow=1358.00 rsi=50.00 indicators_ready=no signal=NO_ENTRY reason="indicator warm-up" quote_age_ms=1[worker] symbol=RELIANCE state=Monitoring last=1358.00 bid=1357.80 ask=1358.00 bid_depth_qty=2078 ask_depth_qty=5642 imbalance=-0.462 ema_fast=1358.00 ema_slow=1358.00 rsi=100.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=1020
[worker] symbol=RELIANCE state=Monitoring last=1357.90 bid=1357.70 ask=1357.90 bid_depth_qty=1485 ask_depth_qty=10527 imbalance=-0.753 ema_fast=1357.97 ema_slow=1357.99 rsi=0.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=952
[worker] symbol=RELIANCE state=Monitoring last=1357.80 bid=1357.60 ask=1357.80 bid_depth_qty=5395 ask_depth_qty=1589 imbalance=0.545 ema_fast=1357.91 ema_slow=1357.97 rsi=0.00 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=908
[worker] symbol=RELIANCE state=Monitoring last=1358.10 bid=1358.00 ask=1358.50 bid_depth_qty=7165 ask_depth_qty=11620 imbalance=-0.237 ema_fast=1357.97 ema_slow=1357.98 rsi=31.58 indicators_ready=yes signal=NO_ENTRY reason="fast EMA is not above slow EMA" quote_age_ms=872
[worker] symbol=RELIANCE state=Monitoring last=1358.50 bid=1358.30 ask=1358.50 bid_depth_qty=5544 ask_depth_qty=739 imbalance=0.765 ema_fast=1358.15 ema_slow=1358.03 rsi=53.95 indicators_ready=yes signal=NO_ENTRY reason="RSI below entry range" quote_age_ms=818
[worker] symbol=RELIANCE state=Monitoring last=1358.60 bid=1358.60 ask=1358.70 bid_depth_qty=5171 ask_depth_qty=479 imbalance=0.830 ema_fast=1358.30 ema_slow=1358.09 rsi=57.92 indicators_ready=yes signal=ENTRY reason="all entry filters passed" quote_age_ms=762
[worker] symbol=RELIANCE order=not_placed reason="Live Kite orders are disabled" state=Cooldown
[worker] symbol=RELIANCE state=Monitoring last=1358.70 bid=1358.60 ask=1358.70 bid_depth_qty=5186 ask_depth_qty=717 imbalance=0.757 ema_fast=1358.43 ema_slow=1358.15 rsi=61.73 indicators_ready=yes signal=ENTRY reason="all entry filters passed" quote_age_ms=667
[worker] symbol=RELIANCE order=not_placed reason="Live Kite orders are disabled" state=Cooldown
[worker] symbol=RELIANCE state=Monitoring last=1358.50 bid=1358.50 ask=1358.80 bid_depth_qty=6342 ask_depth_qty=1387 imbalance=0.641 ema_fast=1358.46 ema_slow=1358.18 rsi=51.07 indicators_ready=yes signal=NO_ENTRY reason="RSI below entry range" quote_age_ms=571
[worker] symbol=RELIANCE state=Monitoring last=1358.50 bid=1358.60 ask=1359.10 bid_depth_qty=6236 ask_depth_qty=8170 imbalance=-0.134 ema_fast=1358.47 ema_slow=1358.21 rsi=51.07 indicators_ready=yes signal=NO_ENTRY reason="RSI below entry range" quote_age_ms=537
[worker] symbol=RELIANCE state=Monitoring last=1358.70 bid=1358.70 ask=1359.10 bid_depth_qty=7098 ask_depth_qty=2770 imbalance=0.439 ema_fast=1358.55 ema_slow=1358.26 rsi=60.21 indicators_ready=yes signal=ENTRY reason="all entry filters passed" quote_age_ms=512
[worker] symbol=RELIANCE order=not_placed reason="Live Kite orders are disabled" state=Cooldown
[worker] symbol=RELIANCE state=Monitoring last=1358.70 bid=1358.70 ask=1359.30 bid_depth_qty=6286 ask_depth_qty=9339 imbalance=-0.195 ema_fast=1358.60 ema_slow=1358.30 rsi=60.21 indicators_ready=yes signal=NO_ENTRY reason="bid depth imbalance below threshold" quote_age_ms=420
[worker] symbol=RELIANCE state=Monitoring last=1358.60 bid=1358.60 ask=1359.00 bid_depth_qty=5364 ask_depth_qty=7381 imbalance=-0.158 ema_fast=1358.60 ema_slow=1358.33 rsi=53.55 indicators_ready=yes signal=NO_ENTRY reason="RSI below entry range" quote_age_ms=389
[worker] symbol=RELIANCE state=Monitoring last=1358.50 bid=1358.50 ask=1358.90 bid_depth_qty=3691 ask_depth_qty=7313 imbalance=-0.329 ema_fast=1358.57 ema_slow=1358.34 rsi=47.49 indicators_ready=yes signal=NO_ENTRY reason="RSI below entry range" quote_age_ms=353
[worker] symbol=RELIANCE state=Monitoring last=1358.50 bid=1358.50 ask=1359.00 bid_depth_qty=7778 ask_depth_qty=11778 imbalance=-0.205 ema_fast=1358.54 ema_slow=1358.36 rsi=47.49 indicators_ready=yes signal=NO_ENTRY reason="RSI below entry range" quote_age_ms=312
[worker] symbol=RELIANCE state=Monitoring last=1358.60 bid=1358.60 ask=1358.70 bid_depth_qty=3034 ask_depth_qty=10487 imbalance=-0.551 ema_fast=1358.56 ema_slow=1358.38 rsi=54.37 indicators_ready=yes signal=NO_ENTRY reason="RSI below entry range" quote_age_ms=271
[worker] symbol=RELIANCE state=Monitoring last=1358.60 bid=1358.60 ask=1358.90 bid_depth_qty=6922 ask_depth_qty=5848 imbalance=0.084 ema_fast=1358.58 ema_slow=1358.40 rsi=54.37 indicators_ready=yes signal=NO_ENTRY reason="RSI below entry range" quote_age_ms=229
[worker] symbol=RELIANCE state=Monitoring last=1358.60 bid=1358.60 ask=1359.00 bid_depth_qty=7423 ask_depth_qty=16853 imbalance=-0.388 ema_fast=1358.58 ema_slow=1358.42 rsi=54.37 indicators_ready=yes signal=NO_ENTRY reason="RSI below entry range" quote_age_ms=187
[worker] symbol=RELIANCE state=Monitoring last=1359.00 bid=1358.60 ask=1359.00 bid_depth_qty=8163 ask_depth_qty=16663 imbalance=-0.342 ema_fast=1358.72 ema_slow=1358.48 rsi=74.72 indicators_ready=yes signal=NO_ENTRY reason="RSI above entry range" quote_age_ms=146
[worker] symbol=RELIANCE state=Monitoring last=1358.60 bid=1358.60 ask=1358.80 bid_depth_qty=7295 ask_depth_qty=5858 imbalance=0.109 ema_fast=1358.68 ema_slow=1358.49 rsi=49.33 indicators_ready=yes signal=NO_ENTRY reason="RSI below entry range" quote_age_ms=112
Shutdown requested; stopping workers
PS C:\Users\Abhishek Tyagi\Desktop\ZeroTick-Systems\ZeroTick-Systems-1> 

19th Nov 3rd testing

Starting Kite login in your browser...
Waiting for the Kite login callback on localhost...
Kite login callback received; exchanging token.
Kite access token obtained and saved in local .env
[kite] quote_batch=received instruments=20
Started 1 of 20 stock worker threads; 19 disabled; active_symbol=RELIANCE
[worker] symbol=RELIANCE state=Monitoring last=1360.70 bid=1360.70 ask=1360.90 bid_depth_qty=7148 ask_depth_qty=6501 weighted_imbalance=-0.113 spread_pct=0.015 price_change_pct=0.000 confirmations=0 signal=NO_ENTRY reason="weighted imbalance below +0.25" quote_age_ms=1
[worker] symbol=RELIANCE state=Monitoring last=1360.70 bid=1360.70 ask=1360.90 bid_depth_qty=7148 ask_depth_qty=6501 weighted_imbalance=-0.113 spread_pct=0.015 price_change_pct=0.000 confirmations=0 signal=NO_ENTRY reason="weighted imbalance below +0.25" quote_age_ms=1007
[worker] symbol=RELIANCE state=Monitoring last=1360.50 bid=1360.60 ask=1360.90 bid_depth_qty=7227 ask_depth_qty=6744 weighted_imbalance=-0.032 spread_pct=0.022 price_change_pct=-0.015 confirmations=0 signal=NO_ENTRY reason="weighted imbalance below +0.25" quote_age_ms=967
[worker] symbol=RELIANCE state=Monitoring last=1360.60 bid=1360.50 ask=1360.70 bid_depth_qty=7871 ask_depth_qty=6492 weighted_imbalance=0.180 spread_pct=0.015 price_change_pct=0.007 confirmations=0 signal=NO_ENTRY reason="weighted imbalance below +0.25" quote_age_ms=810
[worker] symbol=RELIANCE state=Monitoring last=1360.70 bid=1360.60 ask=1360.70 bid_depth_qty=5710 ask_depth_qty=6630 weighted_imbalance=0.001 spread_pct=0.007 price_change_pct=0.007 confirmations=0 signal=NO_ENTRY reason="weighted imbalance below +0.25" quote_age_ms=762
[worker] symbol=RELIANCE state=Monitoring last=1361.00 bid=1360.80 ask=1361.00 bid_depth_qty=5256 ask_depth_qty=6279 weighted_imbalance=-0.345 spread_pct=0.015 price_change_pct=0.022 confirmations=0 signal=NO_ENTRY reason="weighted imbalance below +0.25" quote_age_ms=734
[worker] symbol=RELIANCE state=Monitoring last=1361.00 bid=1360.70 ask=1361.00 bid_depth_qty=7863 ask_depth_qty=6617 weighted_imbalance=-0.103 spread_pct=0.022 price_change_pct=0.000 confirmations=0 signal=NO_ENTRY reason="weighted imbalance below +0.25" quote_age_ms=665
[worker] symbol=RELIANCE state=Monitoring last=1360.80 bid=1360.80 ask=1361.00 bid_depth_qty=8991 ask_depth_qty=6205 weighted_imbalance=0.071 spread_pct=0.015 price_change_pct=-0.015 confirmations=0 signal=NO_ENTRY reason="weighted imbalance below +0.25" quote_age_ms=620
[worker] symbol=RELIANCE state=Monitoring last=1361.20 bid=1361.10 ask=1361.20 bid_depth_qty=6114 ask_depth_qty=3423 weighted_imbalance=0.107 spread_pct=0.007 price_change_pct=0.029 confirmations=0 signal=NO_ENTRY reason="weighted imbalance below +0.25" quote_age_ms=578
[worker] symbol=RELIANCE state=Monitoring last=1361.20 bid=1361.20 ask=1361.40 bid_depth_qty=7740 ask_depth_qty=993 weighted_imbalance=0.767 spread_pct=0.015 price_change_pct=0.000 confirmations=1 signal=NO_ENTRY reason="imbalance confirmation 1/3" quote_age_ms=530
[worker] symbol=RELIANCE state=Monitoring last=1361.20 bid=1361.20 ask=1361.30 bid_depth_qty=4042 ask_depth_qty=947 weighted_imbalance=0.592 spread_pct=0.007 price_change_pct=0.000 confirmations=2 signal=NO_ENTRY reason="imbalance confirmation 2/3" quote_age_ms=480
[worker] symbol=RELIANCE state=Monitoring last=1361.30 bid=1361.20 ask=1361.40 bid_depth_qty=6898 ask_depth_qty=2531 weighted_imbalance=0.330 spread_pct=0.015 price_change_pct=0.007 confirmations=3 signal=ENTRY reason="imbalance confirmed for 3 consecutive snapshots" quote_age_ms=432
[worker] symbol=RELIANCE order=not_placed reason="Live Kite orders are disabled" state=Cooldown[worker] symbol=RELIANCE state=Monitoring last=1361.00 bid=1360.90 ask=1361.00 bid_depth_qty=8668 ask_depth_qty=2672 weighted_imbalance=0.404 spread_pct=0.007 price_change_pct=-0.022 confirmations=0 signal=NO_ENTRY reason="last price fell from previous snapshot" quote_age_ms=349
[worker] symbol=RELIANCE state=Monitoring last=1361.00 bid=1360.80 ask=1361.00 bid_depth_qty=10073 ask_depth_qty=1399 weighted_imbalance=0.826 spread_pct=0.015 price_change_pct=0.000 confirmations=1 signal=NO_ENTRY reason="imbalance confirmation 1/3" quote_age_ms=293
[worker] symbol=RELIANCE state=Monitoring last=1360.70 bid=1360.80 ask=1361.00 bid_depth_qty=9138 ask_depth_qty=1405 weighted_imbalance=0.807 spread_pct=0.015 price_change_pct=-0.022 confirmations=0 signal=NO_ENTRY reason="last price fell from previous snapshot" quote_age_ms=251
[worker] symbol=RELIANCE state=Monitoring last=1360.80 bid=1360.90 ask=1361.00 bid_depth_qty=8201 ask_depth_qty=1688 weighted_imbalance=0.732 spread_pct=0.007 price_change_pct=0.007 confirmations=1 signal=NO_ENTRY reason="imbalance confirmation 1/3" quote_age_ms=196
[worker] symbol=RELIANCE state=Monitoring last=1360.20 bid=1360.20 ask=1360.60 bid_depth_qty=7654 ask_depth_qty=3615 weighted_imbalance=0.464 spread_pct=0.029 price_change_pct=-0.044 confirmations=0 signal=NO_ENTRY reason="last price fell from previous snapshot" quote_age_ms=151
[worker] symbol=RELIANCE state=Monitoring last=1360.20 bid=1360.20 ask=1360.60 bid_depth_qty=7654 ask_depth_qty=3615 weighted_imbalance=0.464 spread_pct=0.029 price_change_pct=0.000 confirmations=1 signal=NO_ENTRY reason="imbalance confirmation 1/3" quote_age_ms=1176
Shutdown requested; stopping workers
