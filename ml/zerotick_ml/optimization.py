import json
from pathlib import Path

import optuna

from .events import DataError, session_order
from .replay import apply_parameters, replay_shadow


INTEGER_PARAMETERS = {
    "selection.momentum_lookback_seconds",
    "selection.trend_lookback_seconds",
    "decision.required_confirmations",
    "handling.max_holding_seconds",
    "handling.reversal_confirmations"
}


def split_events_by_session(events, validation_fraction, test_fraction):
    sessions = session_order(events)
    if len(sessions) < 3:
        raise DataError("At least three shadow sessions are required for optimization")
    test_count = max(1, int(round(len(sessions) * test_fraction)))
    validation_count = max(1, int(round(len(sessions) * validation_fraction)))
    if test_count + validation_count >= len(sessions):
        test_count = validation_count = 1
    train_session_list = sessions[:len(sessions) - validation_count - test_count]
    validation_session_list = sessions[
        len(train_session_list):len(sessions) - test_count]
    test_session_list = sessions[len(sessions) - test_count:]
    train_sessions = set(train_session_list)
    validation_sessions = set(validation_session_list)
    test_sessions = set(test_session_list)

    def subset(selected):
        return [event for event in events if event.get("session_id") in selected]

    return (subset(train_sessions), subset(validation_sessions),
            subset(test_sessions), {
                "train": train_session_list,
                "validation": validation_session_list,
                "test": test_session_list})


def suggest_parameters(trial, search_space, include_model):
    parameters = {}
    for name, bounds in search_space.items():
        if name.startswith("model.") and not include_model:
            continue
        low, high = bounds
        if name in INTEGER_PARAMETERS:
            parameters[name] = trial.suggest_int(name, int(low), int(high))
        else:
            parameters[name] = trial.suggest_float(name, float(low), float(high))
    return parameters


def replay_score(result, objective_config):
    minimum_trades = int(objective_config.get("minimum_trades", 20))
    missing = max(0, minimum_trades - result["closed_trades"])
    return (result["net_return_percent"] -
            float(objective_config.get("drawdown_penalty", 2.0)) *
            result["max_drawdown_percent"] -
            missing * float(objective_config.get(
                "insufficient_trade_penalty", 5.0)))


def optimize(events, trading_config, pipeline_config, weights, output_dir,
             model=None):
    available_sessions = session_order(events)
    minimum_sessions = int(pipeline_config.get("minimum_sessions", 5))
    if len(available_sessions) < minimum_sessions:
        raise DataError("Need at least {} sessions for optimization; found {}".format(
            minimum_sessions, len(available_sessions)))
    train_events, validation_events, test_events, sessions = \
        split_events_by_session(
            events,
            float(pipeline_config.get("validation_session_fraction", 0.2)),
            float(pipeline_config.get("test_session_fraction", 0.2)))
    if not any(event.get("event_type") == "market_snapshot"
               for event in train_events):
        raise DataError("Optimization requires market_snapshot events")
    search_space = pipeline_config.get("search_space", {})
    objective_config = pipeline_config.get("objective", {})
    seed = int(pipeline_config.get("random_seed", 42))
    study = optuna.create_study(
        direction="maximize",
        sampler=optuna.samplers.TPESampler(seed=seed, multivariate=True))

    def objective(trial):
        parameters = suggest_parameters(trial, search_space, model is not None)
        probability = float(parameters.get(
            "model.minimum_probability",
            pipeline_config.get("minimum_model_probability", 0.55)))
        result = replay_shadow(
            train_events, trading_config, weights, parameters,
            model=model, probability_threshold=probability)
        trial.set_user_attr("closed_trades", result["closed_trades"])
        trial.set_user_attr("net_profit_loss", result["net_profit_loss"])
        trial.set_user_attr("max_drawdown_percent",
                            result["max_drawdown_percent"])
        return replay_score(result, objective_config)

    study.optimize(objective, n_trials=int(pipeline_config.get("optuna_trials", 100)))
    best = dict(study.best_trial.params)
    probability = float(best.get(
        "model.minimum_probability",
        pipeline_config.get("minimum_model_probability", 0.55)))
    reports = {}
    for name, subset in (("train", train_events),
                         ("validation", validation_events),
                         ("test", test_events)):
        reports[name] = replay_shadow(
            subset, trading_config, weights, best,
            model=model, probability_threshold=probability)
        reports[name].pop("trades", None)

    suggested_config = apply_parameters(trading_config, best)
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    phase = "model_assisted" if model is not None else "strategy"
    with (output / (phase + "_suggested_config.json")).open(
            "w", encoding="utf-8") as stream:
        json.dump(suggested_config, stream, indent=2)
    report = {
        "phase": phase,
        "best_score": study.best_value,
        "best_parameters": best,
        "sessions": sessions,
        "results": reports,
        "trials": len(study.trials),
        "warning": "Offline recommendation only; config.json was not modified."
    }
    with (output / (phase + "_optimization_report.json")).open(
            "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, default=str)
    study.trials_dataframe().to_csv(
        output / (phase + "_optuna_trials.csv"), index=False)
    return report
