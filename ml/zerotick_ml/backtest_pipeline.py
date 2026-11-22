import argparse
import glob
import hashlib
import json
import math
import sys
from pathlib import Path

import numpy as np
import optuna
import pandas as pd
import xgboost as xgb
from sklearn.metrics import average_precision_score, roc_auc_score

from .events import DataError


FEATURES = [
    "side_short", "relative_volume", "momentum_1m_percent",
    "momentum_2m_percent", "trend_5m_percent", "trend_10m_percent",
    "estimated_spread_percent", "intraday_range_percent", "range_position",
    "body_percent", "bar_range_percent", "log_volume", "time_sin", "time_cos"
]


def _load_json(path):
    with Path(path).open("r", encoding="utf-8-sig") as stream:
        return json.load(stream)


def _paths(root, patterns):
    result = []
    for pattern in patterns:
        candidate = Path(pattern)
        absolute = candidate if candidate.is_absolute() else root / candidate
        result.extend(Path(value) for value in glob.glob(str(absolute)))
    result = sorted(set(path.resolve() for path in result))
    if not result:
        raise DataError("No historical JSONL files match the configured input_globs")
    return result


def load_candles(paths):
    rows = []
    manifests = []
    completed = set()
    for path in paths:
        with path.open("r", encoding="utf-8-sig") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    event = json.loads(line)
                except json.JSONDecodeError as error:
                    raise DataError("{}:{} invalid JSON: {}".format(
                        path, line_number, error)) from error
                if event.get("schema_version") != 1:
                    raise DataError("{}:{} unsupported schema_version".format(
                        path, line_number))
                kind = event.get("event_type")
                if kind == "historical_dataset_started":
                    manifests.append(event)
                elif kind == "historical_dataset_completed":
                    completed.add(event.get("dataset_id"))
                elif kind == "historical_symbol_session":
                    required = ("dataset_id", "split", "session_date", "symbol",
                                "candles")
                    if any(name not in event for name in required):
                        raise DataError("{}:{} incomplete historical record".format(
                            path, line_number))
                    for candle in event["candles"]:
                        rows.append({
                            "dataset_id": event["dataset_id"],
                            "split": event["split"],
                            "session_id": event["session_date"],
                            "symbol": event["symbol"],
                            "tick_size": float(event.get("tick_size", 0.05)),
                            "timestamp": candle["timestamp"],
                            "open": float(candle["open"]),
                            "high": float(candle["high"]),
                            "low": float(candle["low"]),
                            "close": float(candle["close"]),
                            "volume": float(candle["volume"])
                        })
    if not manifests:
        raise DataError("Historical dataset manifest is missing")
    missing = {item.get("dataset_id") for item in manifests} - completed
    if missing:
        raise DataError("Dataset is incomplete: {}".format(sorted(missing)))
    frame = pd.DataFrame(rows)
    if frame.empty:
        raise DataError("Historical dataset contains no candles")
    frame["timestamp"] = pd.to_datetime(frame["timestamp"], errors="raise")
    frame.sort_values(["session_id", "symbol", "timestamp"], inplace=True)
    train = list(dict.fromkeys(frame.loc[frame["split"] == "train", "session_id"]))
    validation = list(dict.fromkeys(
        frame.loc[frame["split"] == "validation", "session_id"]))
    if not train or not validation or set(train) & set(validation):
        raise DataError("Dataset needs disjoint non-empty train and validation sessions")
    if max(train) >= min(validation):
        raise DataError("All training sessions must chronologically precede validation")
    return frame, train, validation


def inspect_dataset(paths):
    manifests = []
    completed = set()
    sessions = {"train": set(), "validation": set()}
    candle_count = 0
    record_count = 0
    for path in paths:
        with path.open("r", encoding="utf-8-sig") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    event = json.loads(line)
                except json.JSONDecodeError as error:
                    raise DataError("{}:{} invalid JSON: {}".format(
                        path, line_number, error)) from error
                if event.get("schema_version") != 1:
                    raise DataError("{}:{} unsupported schema_version".format(
                        path, line_number))
                kind = event.get("event_type")
                if kind == "historical_dataset_started":
                    manifests.append(event)
                elif kind == "historical_dataset_completed":
                    completed.add(event.get("dataset_id"))
                elif kind == "historical_symbol_session":
                    split = event.get("split")
                    if split not in sessions:
                        raise DataError("{}:{} invalid split".format(path, line_number))
                    if not all(name in event for name in
                               ("dataset_id", "session_date", "symbol", "candles")):
                        raise DataError("{}:{} incomplete historical record".format(
                            path, line_number))
                    sessions[split].add(event["session_date"])
                    candle_count += len(event["candles"])
                    record_count += 1
    if not manifests:
        raise DataError("Historical dataset manifest is missing")
    missing = {item.get("dataset_id") for item in manifests} - completed
    if missing:
        raise DataError("Dataset is incomplete: {}".format(sorted(missing)))
    train = sorted(sessions["train"])
    validation = sorted(sessions["validation"])
    if not train or not validation or set(train) & set(validation):
        raise DataError("Dataset needs disjoint non-empty train and validation sessions")
    if max(train) >= min(validation):
        raise DataError("All training sessions must chronologically precede validation")
    return {"candles": candle_count, "records": record_count,
            "train_sessions": train, "validation_sessions": validation}


def stream_candidates(paths, trading, holding_minutes):
    frames = []
    for path in paths:
        with path.open("r", encoding="utf-8-sig") as stream:
            for line in stream:
                if not line.strip():
                    continue
                event = json.loads(line)
                if event.get("event_type") != "historical_symbol_session":
                    continue
                rows = [{
                    "dataset_id": event["dataset_id"], "split": event["split"],
                    "session_id": event["session_date"], "symbol": event["symbol"],
                    "tick_size": float(event.get("tick_size", 0.05)),
                    "timestamp": candle["timestamp"], "open": float(candle["open"]),
                    "high": float(candle["high"]), "low": float(candle["low"]),
                    "close": float(candle["close"]),
                    "volume": float(candle["volume"])
                } for candle in event["candles"]]
                if not rows:
                    continue
                frame = pd.DataFrame(rows)
                frame["timestamp"] = pd.to_datetime(frame["timestamp"], errors="raise")
                candidate = make_candidates(frame, trading, holding_minutes)
                if not candidate.empty:
                    frames.append(candidate)
    if not frames:
        raise DataError("Historical dataset produced no candidate rows")
    return pd.concat(frames, ignore_index=True)


def make_candidates(frame, trading, holding_minutes):
    backtest = trading.get("backtest", {})
    selection = trading.get("selection", {})
    base_spread = float(backtest.get("synthetic_base_spread_percent", 0.02))
    spread_factor = float(
        backtest.get("synthetic_volatility_spread_factor", 0.05))
    maximum_spread = float(
        backtest.get("synthetic_maximum_spread_percent", 0.20))
    start = backtest.get("entry_start_time", "10:00")
    end = backtest.get("entry_end_time", "14:45")
    groups = frame.groupby(["session_id", "symbol"], sort=False, group_keys=False)
    result = frame.copy()
    result["momentum_1m_percent"] = groups["close"].pct_change(1) * 100.0
    result["momentum_2m_percent"] = groups["close"].pct_change(2) * 100.0
    result["trend_5m_percent"] = groups["close"].pct_change(5) * 100.0
    result["trend_10m_percent"] = groups["close"].pct_change(10) * 100.0
    result["rolling_volume"] = groups["volume"].transform(
        lambda values: values.rolling(20, min_periods=5).median().shift(1))
    result["relative_volume"] = result["volume"] / result["rolling_volume"]
    result["session_high"] = groups["high"].cummax()
    result["session_low"] = groups["low"].cummin()
    result["intraday_range_percent"] = (
        (result["session_high"] - result["session_low"]) /
        result["session_low"] * 100.0)
    result["range_position"] = np.where(
        result["session_high"] > result["session_low"],
        (result["close"] - result["session_low"]) /
        (result["session_high"] - result["session_low"]), 0.5)
    result["body_percent"] = (
        (result["close"] - result["open"]) / result["open"] * 100.0)
    result["bar_range_percent"] = (
        (result["high"] - result["low"]) / result["low"] * 100.0)
    result["estimated_spread_percent"] = np.minimum(
        maximum_spread, base_spread + result["bar_range_percent"] * spread_factor)
    result["side_short"] = (result["momentum_1m_percent"] < 0).astype(float)
    result["log_volume"] = np.log1p(result["volume"].clip(lower=0))
    seconds = (result["timestamp"].dt.hour * 3600 +
               result["timestamp"].dt.minute * 60 + result["timestamp"].dt.second)
    phase = 2.0 * math.pi * seconds / 86400.0
    result["time_sin"] = np.sin(phase)
    result["time_cos"] = np.cos(phase)
    future_high = groups["high"].transform(
        lambda values: values.iloc[::-1].rolling(
            holding_minutes, min_periods=1).max().iloc[::-1].shift(-1))
    future_low = groups["low"].transform(
        lambda values: values.iloc[::-1].rolling(
            holding_minutes, min_periods=1).min().iloc[::-1].shift(-1))
    future_close = groups["close"].shift(-holding_minutes)
    result["future_up_percent"] = (future_high / result["close"] - 1.0) * 100.0
    result["future_down_percent"] = (1.0 - future_low / result["close"]) * 100.0
    result["future_close_percent"] = (
        (future_close.fillna(groups["close"].transform("last")) /
         result["close"] - 1.0) * 100.0)
    clock = result["timestamp"].dt.strftime("%H:%M")
    floor_momentum = 0.5 * float(selection.get("min_price_change_percent", 0.03))
    keep = (clock >= start) & (clock < end) & (
        result["momentum_1m_percent"].abs() >= floor_momentum)
    result = result.loc[keep].replace([np.inf, -np.inf], np.nan)
    required = FEATURES + ["future_up_percent", "future_down_percent",
                           "future_close_percent"]
    return result.dropna(subset=required).reset_index(drop=True)


def parameters(trial, config, with_model=False):
    values = {}
    for name, bounds in config["search_space"].items():
        if name == "model_minimum_probability" and not with_model:
            continue
        values[name] = trial.suggest_float(name, float(bounds[0]), float(bounds[1]))
    return values


def evaluate(frame, values, objective, probabilities=None):
    direction = np.where(frame["side_short"].to_numpy() > 0, -1.0, 1.0)
    mask = (
        (frame["relative_volume"].to_numpy() >= values["min_relative_volume"]) &
        (np.abs(frame["momentum_1m_percent"].to_numpy()) >= values["min_momentum_percent"]) &
        (np.abs(frame["trend_5m_percent"].to_numpy()) >= values["min_trend_percent"]) &
        (np.sign(frame["momentum_1m_percent"].to_numpy()) ==
         np.sign(frame["trend_5m_percent"].to_numpy())) &
        (frame["intraday_range_percent"].to_numpy() >= values["min_intraday_range_percent"]) &
        (frame["estimated_spread_percent"].to_numpy() <= values["max_spread_percent"]))
    if probabilities is not None:
        mask &= probabilities >= values["model_minimum_probability"]
    selected = frame.loc[mask]
    selected_direction = direction[mask]
    favorable = np.where(
        selected_direction > 0, selected["future_up_percent"],
        selected["future_down_percent"])
    adverse = np.where(
        selected_direction > 0, selected["future_down_percent"],
        selected["future_up_percent"])
    close_return = selected["future_close_percent"].to_numpy() * selected_direction
    target = values["target_percent"]
    stop = values["stop_percent"]
    gross = np.where(adverse >= stop, -stop,
                     np.where(favorable >= target, target, close_return))
    costs = selected["estimated_spread_percent"].to_numpy() + 0.10
    net = gross - costs
    equity = np.cumsum(net)
    peak = np.maximum.accumulate(np.r_[0.0, equity])[1:] if len(equity) else np.array([])
    drawdown = float(np.max(peak - equity)) if len(equity) else 0.0
    trades = int(len(net))
    minimum_trades = int(objective.get("minimum_trades", 100))
    if trades < minimum_trades:
        # A no-trade configuration must never beat a tradable strategy merely
        # by avoiding drawdown. Among infeasible trials, prefer more evidence.
        score = -1.0e9 + trades
    else:
        score = (float(net.sum()) -
                 float(objective.get("drawdown_penalty", 2.0)) * drawdown)
    return {"score": score, "trades": trades,
            "win_rate": float(np.mean(net > 0) * 100.0) if trades else 0.0,
            "net_return_points": float(net.sum()), "max_drawdown_points": drawdown}


def optimize(train, development_validation, config, output, probabilities=None,
             validation_probabilities=None):
    study = optuna.create_study(
        direction="maximize", sampler=optuna.samplers.TPESampler(
            seed=int(config.get("random_seed", 42))))
    objective_config = config.get("objective", {})

    def objective(trial):
        values = parameters(trial, config, probabilities is not None)
        return evaluate(
            development_validation, values, objective_config,
            validation_probabilities)["score"]

    study.optimize(objective, n_trials=int(config.get("optuna_trials", 100)))
    best = dict(study.best_trial.params)
    report = {
        "best_parameters": best, "best_score": study.best_value,
        "development_train": evaluate(train, best, objective_config, probabilities),
        "development_validation": evaluate(
            development_validation, best, objective_config,
            validation_probabilities),
        "trials": len(study.trials)
    }
    output.mkdir(parents=True, exist_ok=True)
    study.trials_dataframe().to_csv(output / "optuna_trials.csv", index=False)
    with (output / "optimization_report.json").open("w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    return best, report


def label_candidates(frame, best):
    direction = np.where(frame["side_short"] > 0, -1.0, 1.0)
    favorable = np.where(direction > 0, frame["future_up_percent"],
                         frame["future_down_percent"])
    adverse = np.where(direction > 0, frame["future_down_percent"],
                       frame["future_up_percent"])
    gross = np.where(adverse >= best["stop_percent"], -best["stop_percent"],
                     np.where(favorable >= best["target_percent"],
                              best["target_percent"],
                              frame["future_close_percent"] * direction))
    labeled = frame.copy()
    labeled["net_profit_loss"] = gross - labeled["estimated_spread_percent"] - 0.10
    labeled["label_profitable"] = (labeled["net_profit_loss"] > 0).astype(int)
    return labeled


def train_model(train, development_validation, config, output):
    if train["label_profitable"].nunique() < 2 or development_validation[
            "label_profitable"].nunique() < 2:
        raise DataError("Both development model splits need winning and losing labels")
    settings = dict(config.get("xgboost", {}))
    settings.update(random_state=int(config.get("random_seed", 42)), n_jobs=-1)
    model = xgb.XGBClassifier(**settings)
    model.fit(train[FEATURES], train["label_profitable"],
              eval_set=[(development_validation[FEATURES],
                         development_validation["label_profitable"])], verbose=False)
    probability = model.predict_proba(development_validation[FEATURES])[:, 1]
    metadata = {
        "feature_columns": FEATURES, "training_rows": len(train),
        "development_validation_rows": len(development_validation),
        "development_validation_roc_auc": float(roc_auc_score(
            development_validation["label_profitable"], probability)),
        "development_validation_average_precision": float(average_precision_score(
            development_validation["label_profitable"], probability)),
        "dataset_sha256": hashlib.sha256(
            train[FEATURES + ["label_profitable"]].to_csv(index=False).encode()).hexdigest()
    }
    output.mkdir(parents=True, exist_ok=True)
    model.save_model(str(output / "xgboost_model.json"))
    with (output / "model_metadata.json").open("w", encoding="utf-8") as stream:
        json.dump(metadata, stream, indent=2)
    pd.DataFrame({"feature": FEATURES, "importance": model.feature_importances_}).sort_values(
        "importance", ascending=False).to_csv(output / "feature_importance.csv", index=False)
    return model, metadata


def run(argv=None):
    parser = argparse.ArgumentParser(description="ZeroTick historical backtest ML pipeline")
    parser.add_argument("command", choices=["validate", "all"])
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--pipeline-config", default="ml/backtest_pipeline_config.json")
    args = parser.parse_args(argv)
    try:
        root = Path(args.root).resolve()
        config_path = Path(args.pipeline_config)
        if not config_path.is_absolute():
            config_path = root / config_path
        config = _load_json(config_path)
        paths = _paths(root, config["input_globs"])
        dataset = inspect_dataset(paths)
        train_sessions = dataset["train_sessions"]
        validation_sessions = dataset["validation_sessions"]
        report = {"files": [str(path) for path in paths],
                  "candles": dataset["candles"], "records": dataset["records"],
                  "training_sessions": len(train_sessions),
                  "validation_sessions": len(validation_sessions),
                  "first_training_session": train_sessions[0],
                  "last_validation_session": validation_sessions[-1]}
        print(json.dumps(report, indent=2))
        if args.command == "validate":
            return 0
        trading_path = Path(config.get("trading_config", "config.json"))
        if not trading_path.is_absolute():
            trading_path = root / trading_path
        output = Path(config.get("output_dir", "ml/artifacts/backtest"))
        if not output.is_absolute():
            output = root / output
        cache_path = output / "historical_candidate_cache.pkl"
        if cache_path.exists():
            print("Loading cached historical candidates: {}".format(cache_path),
                  flush=True)
            candidates = pd.read_pickle(cache_path)
        else:
            candidates = stream_candidates(
                paths, _load_json(trading_path),
                int(config.get("candidate_holding_minutes", 60)))
            output.mkdir(parents=True, exist_ok=True)
            candidates.to_pickle(cache_path)
            print("Cached {} historical candidates: {}".format(
                len(candidates), cache_path), flush=True)
        if len(candidates) < int(config.get("minimum_candidates", 500)):
            raise DataError("Too few historical candidates: {}".format(len(candidates)))
        dev_count = int(config.get("development_validation_sessions", 20))
        if dev_count <= 0 or dev_count >= len(train_sessions):
            raise DataError("development_validation_sessions must be smaller than training split")
        fit_sessions = set(train_sessions[:-dev_count])
        tune_sessions = set(train_sessions[-dev_count:])
        fit = candidates[candidates["session_id"].isin(fit_sessions)].copy()
        tune = candidates[candidates["session_id"].isin(tune_sessions)].copy()
        untouched = candidates[candidates["session_id"].isin(validation_sessions)].copy()
        print("Phase 1: historical strategy optimization")
        best, phase1 = optimize(fit, tune, config, output / "phase1_strategy")
        print(json.dumps(phase1, indent=2))
        print("Phase 2: XGBoost candidate-quality training")
        labeled_fit = label_candidates(fit, best)
        labeled_tune = label_candidates(tune, best)
        model, metadata = train_model(
            labeled_fit, labeled_tune, config, output / "phase2_model")
        print(json.dumps(metadata, indent=2))
        print("Phase 3: model-assisted historical optimization")
        fit_probability = model.predict_proba(fit[FEATURES])[:, 1]
        tune_probability = model.predict_proba(tune[FEATURES])[:, 1]
        assisted, phase3 = optimize(
            fit, tune, config, output / "phase3_model_assisted",
            fit_probability, tune_probability)
        untouched_probability = model.predict_proba(untouched[FEATURES])[:, 1]
        final = evaluate(untouched, assisted, config.get("objective", {}),
                         untouched_probability)
        final_report = {
            "warning": "Untouched validation was evaluated once after all choices were frozen.",
            "training_sessions": train_sessions,
            "validation_sessions": validation_sessions,
            "final_parameters": assisted,
            "untouched_100_session_result": final,
            "candle_limitations": (
                "No genuine order-book imbalance or queue position exists in historical candles. "
                "Spread and same-bar target/stop ordering are conservative approximations.")
        }
        output.mkdir(parents=True, exist_ok=True)
        with (output / "final_untouched_validation.json").open(
                "w", encoding="utf-8") as stream:
            json.dump(final_report, stream, indent=2)
        print(json.dumps(final_report, indent=2))
        return 0
    except (DataError, FileNotFoundError, json.JSONDecodeError, ValueError) as error:
        print("backtest pipeline error: {}".format(error), file=sys.stderr)
        return 2
