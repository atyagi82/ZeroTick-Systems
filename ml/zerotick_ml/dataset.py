import json
from pathlib import Path

import pandas as pd

from .events import DataError
from .features import FEATURE_COLUMNS, market_features


def load_trading_config(path):
    with Path(path).open("r", encoding="utf-8") as stream:
        return json.load(stream)


def estimated_charges(side, entry_price, exit_price, quantity, config):
    costs = config.get("backtest", {})
    entry_value = entry_price * quantity
    exit_value = exit_price * quantity
    buy_value = entry_value if side == "LONG" else exit_value
    sell_value = exit_value if side == "LONG" else entry_value

    rate = float(costs.get("brokerage_rate_percent", 0.03)) / 100.0
    cap = float(costs.get("brokerage_cap_per_order", 20.0))

    def brokerage(value):
        amount = value * rate
        return min(amount, cap) if cap > 0 else amount

    brokerage_total = brokerage(entry_value) + brokerage(exit_value)
    turnover = buy_value + sell_value
    stt = sell_value * float(costs.get("stt_sell_rate_percent", 0.025)) / 100.0
    exchange = turnover * float(costs.get("exchange_rate_percent", 0.00307)) / 100.0
    sebi = turnover * float(costs.get("sebi_rate_percent", 0.0001)) / 100.0
    stamp = buy_value * float(costs.get("stamp_buy_rate_percent", 0.003)) / 100.0
    gst_base = brokerage_total + exchange + sebi
    gst = gst_base * float(costs.get("gst_percent", 18.0)) / 100.0
    return brokerage_total + stt + exchange + sebi + stamp + gst


def build_trade_dataset(events, trading_config, weights):
    decisions = {}
    entries = {}
    rows = []
    for event in events:
        event_type = event.get("event_type")
        signal_id = event.get("signal_id")
        if event_type == "trade_decision" and event.get("accepted") and signal_id:
            decisions[signal_id] = event
        elif event_type == "entry_filled" and signal_id:
            entries[signal_id] = event
        elif event_type == "position_closed" and signal_id:
            decision = decisions.get(signal_id)
            entry = entries.get(signal_id)
            if not decision or not entry:
                continue
            side = entry.get("side") or decision.get(
                "candidate_features", {}).get("side", "LONG")
            entry_price = float(entry.get("price", 0.0))
            exit_price = float(event.get("price", 0.0))
            quantity = int(event.get("quantity") or entry.get("quantity") or 0)
            if entry_price <= 0 or exit_price <= 0 or quantity <= 0:
                continue
            gross = ((exit_price - entry_price) if side == "LONG"
                     else (entry_price - exit_price)) * quantity
            charges = estimated_charges(
                side, entry_price, exit_price, quantity, trading_config)
            net = gross - charges
            turnover = entry_price * quantity
            feature_values = market_features(
                decision.get("market", {}), side, decision["_time"], weights,
                decision.get("candidate_features", {}),
                decision.get("order_plan", {}))
            row = dict(feature_values)
            row.update({
                "session_id": event.get("session_id"),
                "signal_id": signal_id,
                "symbol": entry.get("symbol", ""),
                "decision_time_utc": decision["_time"],
                "entry_time_utc": entry["_time"],
                "exit_time_utc": event["_time"],
                "exit_reason": event.get("reason", ""),
                "entry_price": entry_price,
                "exit_price": exit_price,
                "quantity": quantity,
                "gross_profit_loss": gross,
                "estimated_charges": charges,
                "net_profit_loss": net,
                "net_return_percent": net / turnover * 100.0,
                "label_profitable": 1 if net > 0 else 0
            })
            rows.append(row)
    if not rows:
        raise DataError(
            "No completed shadow trades could be joined through signal_id")
    frame = pd.DataFrame(rows).sort_values("decision_time_utc").reset_index(drop=True)
    missing = [column for column in FEATURE_COLUMNS if column not in frame.columns]
    if missing:
        raise DataError("Training features are missing: " + ", ".join(missing))
    return frame
