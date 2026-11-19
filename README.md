
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

The executable authenticates with Kite Connect and fetches the 20-symbol quote/depth batch once per second. All 20 workers process fresh snapshots from that shared cache. Without the live-trading flag, they only evaluate and log signals. Repeated reads of the same cached snapshot do not count as additional strategy confirmations.

The active worker writes a synchronized status line showing symbol, last price, best bid/ask, five-level depth totals, weighted imbalance, spread, price change, entry trigger/limit, confirmation count, quote age, signal, and the first unmet entry/exit condition. Live-order logs also show quantity, free cash, and estimated notional. Expect roughly one strategy status line per one-second quote refresh; quote/API failures are also logged.

### Local Kite credentials

Copy `.env.example` to `.env` and fill in the API key and secret locally. `.env` is excluded by `.gitignore`; never commit it or paste its contents into chat. When no access token is present, or when launched with `--login`, the app captures a fresh request token through the local browser callback, exchanges it, and stores the access token in `.env`. The request token is kept in memory only. Access tokens are sensitive; revoke/regenerate them when needed. Run the program from the repository root so it finds `.env`.
