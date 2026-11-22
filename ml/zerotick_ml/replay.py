import copy
import math
from collections import defaultdict, deque

from .dataset import estimated_charges
from .features import FEATURE_COLUMNS, market_features, spread_percent, weighted_imbalance


def nested_set(config, dotted_key, value):
    target = config
    parts = dotted_key.split(".")
    for part in parts[:-1]:
        target = target.setdefault(part, {})
    target[parts[-1]] = value


def apply_parameters(base_config, parameters):
    result = copy.deepcopy(base_config)
    for key, value in parameters.items():
        if not key.startswith("model."):
            nested_set(result, key, value)
    return result


def infer_tick(market, fallback):
    prices = []
    for name in ("bid_levels", "ask_levels"):
        for level in market.get(name, []):
            price = float(level.get("price", 0.0) or 0.0)
            if price > 0:
                prices.append(price)
    prices = sorted(set(prices))
    steps = [right - left for left, right in zip(prices, prices[1:])
             if right - left > 1e-8]
    return min(steps) if steps else fallback


def round_up(value, tick):
    return math.ceil(value / tick - 1e-9) * tick


def round_down(value, tick):
    return math.floor(value / tick + 1e-9) * tick


def lookup_change(history, now, lookback_seconds, current):
    target = now.timestamp() - lookback_seconds
    for timestamp, price in reversed(history):
        if timestamp <= target and price > 0:
            return (current - price) / price * 100.0
    return None


def max_drawdown(equity_curve):
    peak = None
    result = 0.0
    for value in equity_curve:
        peak = value if peak is None else max(peak, value)
        if peak > 0:
            result = max(result, (peak - value) / peak * 100.0)
    return result


def replay_shadow(events, base_config, weights, parameters=None,
                  model=None, probability_threshold=0.0):
    ordered_sessions = list(dict.fromkeys(
        event.get("session_id") for event in events
        if event.get("event_type") == "market_snapshot" and
        event.get("session_id")))
    if len(ordered_sessions) > 1:
        session_results = []
        for session_id in ordered_sessions:
            session_results.append(replay_shadow(
                [event for event in events
                 if event.get("session_id") == session_id],
                base_config, weights, parameters, model,
                probability_threshold))
        initial_cash = float(base_config.get("risk", {}).get(
            "paper_trading_cash", 1000000.0))
        trades = [trade for result in session_results
                  for trade in result["trades"]]
        net = sum(result["net_profit_loss"] for result in session_results)
        return {
            "initial_cash": initial_cash,
            "ending_cash": initial_cash + net,
            "net_profit_loss": net,
            "net_return_percent": net / initial_cash * 100.0,
            "max_drawdown_percent": max(
                result["max_drawdown_percent"] for result in session_results),
            "trades": trades,
            "closed_trades": len(trades),
            "winners": sum(1 for trade in trades
                           if trade["net_profit_loss"] > 0)
        }
    config = apply_parameters(base_config, parameters or {})
    selection = config.get("selection", {})
    decision = config.get("decision", {})
    handling = config.get("handling", {})
    risk = config.get("risk", {})
    market_data = config.get("market_data", {})
    states = defaultdict(lambda: {
        "history": deque(), "previous_volume": None, "volume_ewma": None,
        "confirmation_side": None, "confirmations": 0
    })
    pending = {}
    positions = {}
    trades = []
    initial_cash = float(risk.get("paper_trading_cash", 1000000.0))
    cash = initial_cash
    equity_curve = [cash]
    max_positions = int(risk.get("max_open_positions", 20))
    allow_shorts = bool(config.get("allow_short_selling", False))
    fallback_tick = float(market_data.get("fallback_tick_size", 0.05))
    entry_timeout = float(handling.get("entry_fill_timeout_seconds", 15))
    maximum_holding = float(handling.get("max_holding_seconds", 0))
    reversal_threshold = float(
        handling.get("reversal_imbalance_threshold", 0.0))
    reversal_needed = int(handling.get("reversal_confirmations", 1))
    momentum_lookback = int(selection.get("momentum_lookback_seconds", 30))
    trend_lookback = int(selection.get("trend_lookback_seconds", 120))
    longest_lookback = max(momentum_lookback, trend_lookback)

    snapshots = [event for event in events
                 if event.get("event_type") == "market_snapshot"]
    for event in snapshots:
        market = event.get("market", {})
        symbol = market.get("symbol")
        if not symbol:
            continue
        now = event["_time"]
        last = float(market.get("last_price", 0.0) or 0.0)
        bid = float(market.get("bid_price", 0.0) or 0.0)
        ask = float(market.get("ask_price", 0.0) or 0.0)
        if last <= 0:
            continue

        order = pending.get(symbol)
        if order:
            age = (now - order["signal_time"]).total_seconds()
            if age > entry_timeout:
                pending.pop(symbol, None)
            else:
                trigger_reached = (last >= order["trigger"] if order["side"] == "LONG"
                                   else last <= order["trigger"])
                executable = ask if order["side"] == "LONG" else bid
                within_limit = executable > 0 and (
                    executable <= order["limit"] if order["side"] == "LONG"
                    else executable >= order["limit"])
                if trigger_reached and within_limit:
                    order["entry_time"] = now
                    order["entry_price"] = executable
                    order["reversal_count"] = 0
                    positions[symbol] = order
                    pending.pop(symbol, None)

        position = positions.get(symbol)
        if position:
            side = position["side"]
            stop_touched = (last <= position["stop"] if side == "LONG"
                            else last >= position["stop"])
            target_touched = (last >= position["target"] if side == "LONG"
                              else last <= position["target"])
            imbalance = weighted_imbalance(market, weights)
            reversing = reversal_threshold > 0 and (
                imbalance <= -reversal_threshold if side == "LONG"
                else imbalance >= reversal_threshold)
            position["reversal_count"] = (
                position["reversal_count"] + 1 if reversing else 0)
            time_exit = maximum_holding > 0 and (
                now - position["entry_time"]).total_seconds() >= maximum_holding
            reversal_exit = position["reversal_count"] >= reversal_needed
            if stop_touched or target_touched or reversal_exit or time_exit:
                exit_price = bid if side == "LONG" else ask
                if exit_price <= 0:
                    exit_price = last
                gross = ((exit_price - position["entry_price"])
                         if side == "LONG" else
                         (position["entry_price"] - exit_price)) * position["quantity"]
                charges = estimated_charges(
                    side, position["entry_price"], exit_price,
                    position["quantity"], config)
                net = gross - charges
                cash += net
                record = dict(position["features"])
                record.update({
                    "session_id": event.get("session_id"),
                    "symbol": symbol,
                    "side": side,
                    "signal_time_utc": position["signal_time"],
                    "entry_time_utc": position["entry_time"],
                    "exit_time_utc": now,
                    "gross_profit_loss": gross,
                    "estimated_charges": charges,
                    "net_profit_loss": net,
                    "net_return_percent": net /
                        (position["entry_price"] * position["quantity"]) * 100.0,
                    "label_profitable": 1 if net > 0 else 0,
                    "exit_reason": ("stop_loss" if stop_touched else
                                    "target" if target_touched else
                                    "imbalance_reversal" if reversal_exit else
                                    "time_exit")
                })
                trades.append(record)
                positions.pop(symbol, None)
                equity_curve.append(cash)

        state = states[(event.get("session_id"), symbol)]
        history = state["history"]
        momentum = lookup_change(history, now, momentum_lookback, last)
        trend = lookup_change(history, now, trend_lookback, last)
        history.append((now.timestamp(), last))
        oldest = now.timestamp() - longest_lookback - 5
        while len(history) > 2 and history[0][0] < oldest:
            history.popleft()

        cumulative_volume = int(market.get("volume", 0) or 0)
        previous_volume = state["previous_volume"]
        interval_volume = (max(0, cumulative_volume - previous_volume)
                           if previous_volume is not None else 0)
        state["previous_volume"] = cumulative_volume
        if interval_volume > 0:
            alpha = float(selection.get("volume_ewma_alpha", 0.2))
            if state["volume_ewma"] is None:
                state["volume_ewma"] = float(interval_volume)
            relative_volume = interval_volume / state["volume_ewma"]
            state["volume_ewma"] = (alpha * interval_volume +
                                      (1.0 - alpha) * state["volume_ewma"])
        else:
            relative_volume = 1.0

        if symbol in pending or symbol in positions or momentum is None or trend is None:
            continue
        side = "SHORT" if momentum < 0 else "LONG"
        if side == "SHORT" and not allow_shorts:
            continue
        high = float(market.get("high", 0.0) or 0.0)
        low = float(market.get("low", 0.0) or 0.0)
        range_percent = (high - low) / low * 100.0 if high > low > 0 else 0.0
        spread = spread_percent(market)
        imbalance = weighted_imbalance(market, weights)
        directional = (
            momentum >= float(selection.get("min_price_change_percent", 0.0)) and
            trend >= float(selection.get("min_trend_change_percent", 0.0)) and
            imbalance >= float(decision.get("minimum_imbalance", 0.0))
            if side == "LONG" else
            momentum <= -float(selection.get("min_price_change_percent", 0.0)) and
            trend <= -float(selection.get("min_trend_change_percent", 0.0)) and
            imbalance <= -float(decision.get("minimum_imbalance", 0.0)))
        eligible = all([
            last > float(selection.get("min_price", 0.0)),
            cumulative_volume >= int(selection.get("min_absolute_volume", 0)),
            relative_volume >= float(selection.get("min_relative_volume", 0.0)),
            relative_volume >= float(decision.get("min_relative_volume", 0.0)),
            spread <= float(selection.get("max_spread_percent", 100.0)),
            spread <= float(decision.get("maximum_spread_percent", 100.0)),
            range_percent >= float(selection.get("min_intraday_range_percent", 0.0)),
            range_percent <= float(selection.get("max_price_change_percent", 100.0)),
            directional
        ])
        if not eligible:
            state["confirmations"] = 0
            state["confirmation_side"] = None
            continue
        if state["confirmation_side"] == side:
            state["confirmations"] += 1
        else:
            state["confirmation_side"] = side
            state["confirmations"] = 1
        if state["confirmations"] < int(decision.get("required_confirmations", 1)):
            continue
        state["confirmations"] = 0

        tick = infer_tick(market, fallback_tick)
        trigger_offset = int(decision.get("entry_trigger_offset_ticks", 0))
        limit_offset = int(decision.get("entry_limit_offset_ticks", 1))
        trigger_base = max(last, ask) if side == "LONG" else min(last, bid)
        trigger = ((round_up(trigger_base, tick) + trigger_offset * tick)
                   if side == "LONG" else
                   (round_down(trigger_base, tick) - trigger_offset * tick))
        limit_price = (trigger + limit_offset * tick if side == "LONG"
                       else trigger - limit_offset * tick)
        target_percent = float(decision.get("target_percent", 0.5))
        stop_percent = float(decision.get("stop_percent", 0.3))
        target = limit_price * (1.0 + target_percent / 100.0
                                if side == "LONG" else
                                1.0 - target_percent / 100.0)
        stop = limit_price * (1.0 - stop_percent / 100.0
                              if side == "LONG" else
                              1.0 + stop_percent / 100.0)
        candidate = {
            "relative_volume": relative_volume,
            "price_change_percent": momentum,
            "trend_change_percent": trend,
            "spread_percent": spread,
            "intraday_range_percent": range_percent
        }
        plan = {"entry_limit": limit_price, "target": target, "stop_loss": stop}
        features = market_features(market, side, now, weights, candidate, plan)
        if model is not None:
            import pandas as pd
            probability = float(model.predict_proba(
                pd.DataFrame([[features[name] for name in FEATURE_COLUMNS]],
                             columns=FEATURE_COLUMNS))[0, 1])
            if probability < probability_threshold:
                continue
        reserved = len(pending) + len(positions)
        if reserved >= max_positions:
            continue
        slot_budget = min(initial_cash / max_positions, max(0.0, cash))
        quantity = int(slot_budget // limit_price)
        indicative = int(float(decision.get("indicative_entry_budget", 50000.0))
                         // limit_price)
        quantity = min(quantity, max(1, indicative))
        if quantity < 1:
            continue
        pending[symbol] = {
            "side": side, "signal_time": now, "trigger": trigger,
            "limit": limit_price, "target": target, "stop": stop,
            "quantity": quantity, "features": features
        }

    return {
        "initial_cash": initial_cash,
        "ending_cash": cash,
        "net_profit_loss": cash - initial_cash,
        "net_return_percent": (cash - initial_cash) / initial_cash * 100.0,
        "max_drawdown_percent": max_drawdown(equity_curve),
        "trades": trades,
        "closed_trades": len(trades),
        "winners": sum(1 for trade in trades if trade["net_profit_loss"] > 0)
    }
