# ZeroTick-Systems

ZeroTick-Systems is a C++ algorithmic trading platform being developed using Zerodha's Kite developer APIs. The project aims to support fast, data-driven trading strategies and execution that seek small but frequent opportunities while maintaining strict risk controls.

> **Status:** Early-stage development. Strategy execution and API integration require further implementation and validation.

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

Detailed build instructions will be added as the C++ project structure and dependencies are established.

## ANM PROJECT ##
