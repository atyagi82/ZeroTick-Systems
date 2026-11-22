import math


FEATURE_COLUMNS = [
    "side_short",
    "relative_volume",
    "momentum_percent",
    "trend_percent",
    "spread_percent",
    "intraday_range_percent",
    "weighted_imbalance",
    "top_level_imbalance",
    "log_bid_depth",
    "log_ask_depth",
    "price_vs_average_percent",
    "price_vs_open_percent",
    "range_position",
    "log_volume",
    "entry_limit_distance_percent",
    "target_distance_percent",
    "stop_distance_percent",
    "time_sin",
    "time_cos"
]


def finite(value, default=0.0):
    try:
        result = float(value)
        return result if math.isfinite(result) else default
    except (TypeError, ValueError):
        return default


def level_quantity(levels, index):
    if not isinstance(levels, list) or index >= len(levels):
        return 0.0
    return max(0.0, finite(levels[index].get("quantity")))


def weighted_imbalance(market, weights):
    bids = market.get("bid_levels", [])
    asks = market.get("ask_levels", [])
    bid = sum(weight * level_quantity(bids, index)
              for index, weight in enumerate(weights))
    ask = sum(weight * level_quantity(asks, index)
              for index, weight in enumerate(weights))
    return (bid - ask) / (bid + ask) if bid + ask > 0.0 else 0.0


def spread_percent(market):
    bid = finite(market.get("bid_price"))
    ask = finite(market.get("ask_price"))
    midpoint = (bid + ask) / 2.0
    return (ask - bid) / midpoint * 100.0 if bid > 0 and ask >= bid else 0.0


def market_features(market, side, event_time, weights, candidate=None,
                    order_plan=None):
    candidate = candidate or {}
    order_plan = order_plan or {}
    last = finite(market.get("last_price"))
    average = finite(market.get("average_price"))
    opened = finite(market.get("open"))
    high = finite(market.get("high"))
    low = finite(market.get("low"))
    bids = market.get("bid_levels", [])
    asks = market.get("ask_levels", [])
    bid_total = sum(level_quantity(bids, index) for index in range(5))
    ask_total = sum(level_quantity(asks, index) for index in range(5))
    top_bid = level_quantity(bids, 0)
    top_ask = level_quantity(asks, 0)
    top_imbalance = ((top_bid - top_ask) / (top_bid + top_ask)
                     if top_bid + top_ask > 0 else 0.0)
    range_position = ((last - low) / (high - low)
                      if high > low and last > 0 else 0.5)
    seconds = event_time.hour * 3600 + event_time.minute * 60 + event_time.second
    phase = 2.0 * math.pi * seconds / 86400.0
    limit_price = finite(order_plan.get("entry_limit"), last)
    target = finite(order_plan.get("target"), limit_price)
    stop = finite(order_plan.get("stop_loss"), limit_price)

    def percent_delta(value, base):
        return (value - base) / base * 100.0 if base > 0 else 0.0

    values = {
        "side_short": 1.0 if side == "SHORT" else 0.0,
        "relative_volume": finite(candidate.get("relative_volume"), 1.0),
        "momentum_percent": finite(candidate.get("price_change_percent")),
        "trend_percent": finite(candidate.get("trend_change_percent")),
        "spread_percent": finite(candidate.get("spread_percent"),
                                   spread_percent(market)),
        "intraday_range_percent": finite(
            candidate.get("intraday_range_percent"),
            percent_delta(high, low) if high > low else 0.0),
        "weighted_imbalance": weighted_imbalance(market, weights),
        "top_level_imbalance": top_imbalance,
        "log_bid_depth": math.log1p(bid_total),
        "log_ask_depth": math.log1p(ask_total),
        "price_vs_average_percent": percent_delta(last, average),
        "price_vs_open_percent": percent_delta(last, opened),
        "range_position": max(0.0, min(1.0, range_position)),
        "log_volume": math.log1p(max(0.0, finite(market.get("volume")))),
        "entry_limit_distance_percent": abs(percent_delta(limit_price, last)),
        "target_distance_percent": abs(percent_delta(target, limit_price)),
        "stop_distance_percent": abs(percent_delta(stop, limit_price)),
        "time_sin": math.sin(phase),
        "time_cos": math.cos(phase)
    }
    return values
