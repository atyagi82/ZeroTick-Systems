import json
import math
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "data" / "backtest" / "backtest_200_sessions.jsonl"
OUTPUT = ROOT / "website" / "dist" / "data"

NIFTY_50 = [
    "ADANIENT", "ADANIPORTS", "APOLLOHOSP", "ASIANPAINT", "AXISBANK",
    "BAJAJ-AUTO", "BAJAJFINSV", "BAJFINANCE", "BEL", "BHARTIARTL", "BSE",
    "CIPLA", "COALINDIA", "DRREDDY", "EICHERMOT", "ETERNAL", "GRASIM",
    "HCLTECH", "HDFCBANK", "HDFCLIFE", "HINDALCO", "HINDUNILVR",
    "ICICIBANK", "INDIGO", "INFY", "ITC", "JIOFIN", "JSWSTEEL",
    "KOTAKBANK", "LT", "M&M", "MARUTI", "MAXHEALTH", "NESTLEIND", "NTPC",
    "ONGC", "POWERGRID", "RELIANCE", "SBILIFE", "SBIN", "SHRIRAMFIN",
    "SUNPHARMA", "TATACONSUM", "TATASTEEL", "TCS", "TECHM", "TITAN",
    "TMPV", "TRENT", "ULTRACEMCO"
]

COMPANIES = {
    "ADANIENT": ["Adani Enterprises", "Metals & Mining"],
    "ADANIPORTS": ["Adani Ports", "Services"],
    "APOLLOHOSP": ["Apollo Hospitals", "Healthcare"],
    "ASIANPAINT": ["Asian Paints", "Consumer"],
    "AXISBANK": ["Axis Bank", "Financial Services"],
    "BAJAJ-AUTO": ["Bajaj Auto", "Automobile"],
    "BAJAJFINSV": ["Bajaj Finserv", "Financial Services"],
    "BAJFINANCE": ["Bajaj Finance", "Financial Services"],
    "BEL": ["Bharat Electronics", "Capital Goods"],
    "BHARTIARTL": ["Bharti Airtel", "Telecommunication"],
    "BSE": ["BSE", "Financial Services"],
    "CIPLA": ["Cipla", "Healthcare"],
    "COALINDIA": ["Coal India", "Energy"],
    "DRREDDY": ["Dr. Reddy's Laboratories", "Healthcare"],
    "EICHERMOT": ["Eicher Motors", "Automobile"],
    "ETERNAL": ["Eternal", "Consumer Services"],
    "GRASIM": ["Grasim Industries", "Diversified"],
    "HCLTECH": ["HCL Technologies", "Information Technology"],
    "HDFCBANK": ["HDFC Bank", "Financial Services"],
    "HDFCLIFE": ["HDFC Life", "Financial Services"],
    "HINDALCO": ["Hindalco Industries", "Metals & Mining"],
    "HINDUNILVR": ["Hindustan Unilever", "FMCG"],
    "ICICIBANK": ["ICICI Bank", "Financial Services"],
    "INDIGO": ["InterGlobe Aviation", "Services"],
    "INFY": ["Infosys", "Information Technology"],
    "ITC": ["ITC", "FMCG"],
    "JIOFIN": ["Jio Financial Services", "Financial Services"],
    "JSWSTEEL": ["JSW Steel", "Metals & Mining"],
    "KOTAKBANK": ["Kotak Mahindra Bank", "Financial Services"],
    "LT": ["Larsen & Toubro", "Construction"],
    "M&M": ["Mahindra & Mahindra", "Automobile"],
    "MARUTI": ["Maruti Suzuki", "Automobile"],
    "MAXHEALTH": ["Max Healthcare", "Healthcare"],
    "NESTLEIND": ["Nestle India", "FMCG"],
    "NTPC": ["NTPC", "Power"],
    "ONGC": ["ONGC", "Energy"],
    "POWERGRID": ["Power Grid", "Power"],
    "RELIANCE": ["Reliance Industries", "Energy"],
    "SBILIFE": ["SBI Life", "Financial Services"],
    "SBIN": ["State Bank of India", "Financial Services"],
    "SHRIRAMFIN": ["Shriram Finance", "Financial Services"],
    "SUNPHARMA": ["Sun Pharma", "Healthcare"],
    "TATACONSUM": ["Tata Consumer", "FMCG"],
    "TATASTEEL": ["Tata Steel", "Metals & Mining"],
    "TCS": ["Tata Consultancy Services", "Information Technology"],
    "TECHM": ["Tech Mahindra", "Information Technology"],
    "TITAN": ["Titan Company", "Consumer"],
    "TMPV": ["Tata Motors Passenger Vehicles", "Automobile"],
    "TRENT": ["Trent", "Retail"],
    "ULTRACEMCO": ["UltraTech Cement", "Construction Materials"]
}

SOURCE_ALLOWLIST = [
    "README.md", "CMakeLists.txt", "config.json",
    "DesignDocuments/High Level Design.md",
    "DesignDocuments/Low Level Design.md",
    "include/zerotick/BacktestEngine.h", "include/zerotick/EngineTypes.h",
    "include/zerotick/KiteApi.h", "include/zerotick/RuntimeConfig.h",
    "include/zerotick/StockSelectionEngine.h",
    "include/zerotick/TradeHandlingEngine.h",
    "include/zerotick/TradingDecisionEngine.h",
    "src/BacktestEngine.cpp", "src/EngineTypes.cpp", "src/KiteConnectApi.cpp",
    "src/RuntimeConfig.cpp", "src/StockSelectionEngine.cpp",
    "src/TradeHandlingEngine.cpp", "src/TradingDecisionEngine.cpp",
    "src/StructuredEventWriter.cpp", "src/main.cpp",
    "ml/README.md", "ml/backtest_pipeline_config.json",
    "ml/pipeline_config.json", "ml/zerotick_ml/backtest_pipeline.py",
    "ml/zerotick_ml/features.py", "ml/zerotick_ml/optimization.py",
    "ml/zerotick_ml/replay.py", "ml/zerotick_ml/training.py"
]


def rsi(closes, period=14):
    values = [None] * len(closes)
    gains, losses = [], []
    for index in range(1, len(closes)):
        change = closes[index] - closes[index - 1]
        gains.append(max(change, 0.0))
        losses.append(max(-change, 0.0))
        if index < period:
            continue
        if index == period:
            average_gain = sum(gains[-period:]) / period
            average_loss = sum(losses[-period:]) / period
        else:
            average_gain = (average_gain * (period - 1) + gains[-1]) / period
            average_loss = (average_loss * (period - 1) + losses[-1]) / period
        values[index] = 100.0 if average_loss == 0 else 100.0 - 100.0 / (
            1.0 + average_gain / average_loss)
    return values


def export_market():
    wanted = set(NIFTY_50)
    series = {symbol: [] for symbol in NIFTY_50}
    with SOURCE.open("r", encoding="utf-8-sig") as stream:
        for line in stream:
            event = json.loads(line)
            if event.get("event_type") != "historical_symbol_session":
                continue
            symbol = event.get("symbol")
            candles = event.get("candles", [])
            if symbol not in wanted or not candles:
                continue
            series[symbol].append({
                "date": event["session_date"],
                "open": candles[0]["open"],
                "high": max(item["high"] for item in candles),
                "low": min(item["low"] for item in candles),
                "close": candles[-1]["close"],
                "volume": sum(item["volume"] for item in candles)
            })

    output = {"generated_from": SOURCE.name, "sessions": 200, "stocks": {}}
    for symbol in NIFTY_50:
        rows = sorted(series[symbol], key=lambda item: item["date"])
        closes = [float(item["close"]) for item in rows]
        rsi_values = rsi(closes)
        for index, row in enumerate(rows):
            start = max(0, index - 19)
            row["sma20"] = round(sum(closes[start:index + 1]) /
                                 (index - start + 1), 2) if index >= 19 else None
            row["rsi14"] = round(rsi_values[index], 2) if rsi_values[index] is not None else None
            for key in ("open", "high", "low", "close"):
                row[key] = round(float(row[key]), 2)
        company, sector = COMPANIES[symbol]
        output["stocks"][symbol] = {
            "name": company, "sector": sector, "history": rows
        }
    (OUTPUT / "market.json").write_text(
        json.dumps(output, separators=(",", ":")), encoding="utf-8")


def export_source():
    files = []
    for relative in SOURCE_ALLOWLIST:
        path = ROOT / relative
        if not path.is_file():
            continue
        content = path.read_text(encoding="utf-8", errors="replace")
        files.append({"path": relative.replace("\\", "/"), "content": content})
    (OUTPUT / "source.json").write_text(
        json.dumps({"files": files}, separators=(",", ":")), encoding="utf-8")


if __name__ == "__main__":
    if not SOURCE.is_file():
        raise SystemExit("Backtest dataset not found: {}".format(SOURCE))
    OUTPUT.mkdir(parents=True, exist_ok=True)
    export_market()
    export_source()
    print("Exported {} NIFTY 50 symbols and {} curated source files".format(
        len(NIFTY_50), len(SOURCE_ALLOWLIST)))
