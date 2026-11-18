# ZeroTick-Systems

ZeroTick-Systems is a C++ algorithmic trading platform being developed using Zerodha's Kite developer APIs. The project aims to support fast, data-driven trading strategies and execution that seek small but frequent opportunities while maintaining strict risk controls.

> **Status:** Prototype. Kite Connect login, batched quote retrieval, and worker status reporting have been exercised locally. Market-hours validation, strategy testing, and production risk controls are still pending. Live order placement is disabled.

## Current status and validation

Validated locally on 2026-11-19:

- The browser login callback completed and the app exchanged the request token for an access token.
- The Kite quote endpoint returned a batch for all 20 configured instruments.
- All 20 workers started and printed market snapshots, depth totals, indicator values, and `NO_ENTRY` reasons.
- The C++ application built successfully with Visual Studio 2022 and CMake.

This run was outside market hours. The returned snapshots showed one-sided or unavailable book levels and repeated prices; this confirms API retrieval, not live, actionable order-book depth. No entry signal fired and no orders were placed. A market-hours validation and paper-trading/backtest evaluation remain necessary.

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

The executable authenticates with Kite Connect, fetches the 20-symbol quote/depth batch once per second, and gives each stock worker its cached snapshot. It does not place live orders.

Each worker writes a synchronized status line showing symbol, last price, best bid/ask, aggregated displayed depth, imbalance, EMA/RSI values, quote age, signal, and the first unmet entry/exit condition. Expect roughly 20 status lines per second; quote/API failures are also logged.

### Local Kite credentials

Copy `.env.example` to `.env` and fill in the API key and secret locally. `.env` is excluded by `.gitignore`; never commit it or paste its contents into chat. When no access token is present, or when launched with `--login`, the app captures a fresh request token through the local browser callback, exchanges it, and stores the access token in `.env`. The request token is kept in memory only. Access tokens are sensitive; revoke/regenerate them when needed. Run the program from the repository root so it finds `.env`.

## Code structure

- `include/zerotick/` - Public C++ interfaces and domain types
- `src/` - State machine, worker, strategy, thread pool, and Kite Connect REST adapter
- `CMakeLists.txt` - Library and executable build configuration

Each worker owns exactly one symbol and follows a state machine:

```text
Idle -> Monitoring -> EntryPending -> PositionOpen -> ExitPending -> Cooldown -> Monitoring
                  \-> Stopped / Error
```

## Basic trading algorithm

`CombinedStrategy` is a long-only scaffold that uses three confirming indicators:

1. **Order-depth imbalance:** `(bid quantity - ask quantity) / (bid quantity + ask quantity)`.
   A value above `+0.20` indicates stronger bid-side demand for entry.
2. **Moving average:** A 5-tick exponential moving average must remain above a 20-tick exponential moving average.
3. **Relative Strength Index:** RSI uses a 14-tick lookback and must be in the `55-70` range.

All three conditions must agree before an entry signal is produced. The position exits on either:

- Profit of `0.25%`
- Loss of `0.15%`
- RSI at or above `75`
- The 5-tick EMA crossing below the 20-tick EMA
- Order imbalance falling to `-0.25` or below

These are unoptimized research values, not profit recommendations. Live order placement remains explicitly disabled in the Kite adapter.


19th November First Testing

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