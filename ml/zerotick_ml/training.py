import hashlib
import json
from pathlib import Path

import numpy as np
import pandas as pd
import xgboost as xgb
from sklearn.metrics import (accuracy_score, average_precision_score, f1_score,
                             log_loss, precision_score, recall_score,
                             roc_auc_score)

from .events import DataError
from .features import FEATURE_COLUMNS


def chronological_session_split(frame, validation_fraction, test_fraction):
    sessions = list(dict.fromkeys(frame["session_id"].tolist()))
    if len(sessions) < 3:
        raise DataError("At least three ordered sessions are required to split data")
    test_count = max(1, int(round(len(sessions) * test_fraction)))
    validation_count = max(1, int(round(len(sessions) * validation_fraction)))
    if test_count + validation_count >= len(sessions):
        validation_count = 1
        test_count = 1
    train_sessions = sessions[:len(sessions) - validation_count - test_count]
    validation_sessions = sessions[len(train_sessions):len(sessions) - test_count]
    test_sessions = sessions[len(sessions) - test_count:]
    return (
        frame[frame["session_id"].isin(train_sessions)].copy(),
        frame[frame["session_id"].isin(validation_sessions)].copy(),
        frame[frame["session_id"].isin(test_sessions)].copy(),
        {"train": train_sessions, "validation": validation_sessions,
         "test": test_sessions})


def classification_metrics(labels, probabilities, threshold):
    predictions = (probabilities >= threshold).astype(int)
    result = {
        "rows": int(len(labels)),
        "positive_rate": float(np.mean(labels)),
        "threshold": float(threshold),
        "accuracy": float(accuracy_score(labels, predictions)),
        "precision": float(precision_score(labels, predictions, zero_division=0)),
        "recall": float(recall_score(labels, predictions, zero_division=0)),
        "f1": float(f1_score(labels, predictions, zero_division=0))
    }
    if len(np.unique(labels)) > 1:
        result["roc_auc"] = float(roc_auc_score(labels, probabilities))
        result["average_precision"] = float(
            average_precision_score(labels, probabilities))
        result["log_loss"] = float(log_loss(labels, probabilities))
    return result


def choose_threshold(frame, probabilities, minimum_trades):
    best = None
    for threshold in np.arange(0.50, 0.801, 0.01):
        accepted = probabilities >= threshold
        count = int(np.sum(accepted))
        if count < minimum_trades:
            continue
        pnl = float(frame.loc[accepted, "net_profit_loss"].sum())
        average = float(frame.loc[accepted, "net_profit_loss"].mean())
        candidate = (pnl, average, -count, float(threshold))
        if best is None or candidate > best:
            best = candidate
    return best[3] if best else 0.5


def train_xgboost(frame, pipeline_config, output_dir):
    minimum_sessions = int(pipeline_config.get("minimum_sessions", 5))
    minimum_trades = int(pipeline_config.get("minimum_completed_trades", 100))
    if frame["session_id"].nunique() < minimum_sessions:
        raise DataError("Need at least {} sessions; found {}".format(
            minimum_sessions, frame["session_id"].nunique()))
    if len(frame) < minimum_trades:
        raise DataError("Need at least {} completed trades; found {}".format(
            minimum_trades, len(frame)))
    train, validation, test, split_sessions = chronological_session_split(
        frame,
        float(pipeline_config.get("validation_session_fraction", 0.2)),
        float(pipeline_config.get("test_session_fraction", 0.2)))
    for name, subset in (("train", train), ("validation", validation),
                         ("test", test)):
        if subset.empty:
            raise DataError(name + " split is empty")
        if subset["label_profitable"].nunique() < 2 and name != "test":
            raise DataError(name + " split needs both winning and losing trades")

    params = dict(pipeline_config.get("xgboost", {}))
    params["random_state"] = int(pipeline_config.get("random_seed", 42))
    params["n_jobs"] = -1
    model = xgb.XGBClassifier(**params)
    x_train = train[FEATURE_COLUMNS]
    y_train = train["label_profitable"].astype(int)
    x_validation = validation[FEATURE_COLUMNS]
    y_validation = validation["label_profitable"].astype(int)
    model.fit(x_train, y_train, eval_set=[(x_validation, y_validation)], verbose=False)

    validation_probability = model.predict_proba(x_validation)[:, 1]
    objective = pipeline_config.get("objective", {})
    threshold = choose_threshold(
        validation, validation_probability,
        int(objective.get("minimum_trades", 20)))
    test_probability = model.predict_proba(test[FEATURE_COLUMNS])[:, 1]
    test_predictions = test.copy()
    test_predictions["model_probability"] = test_probability
    test_predictions["model_accept"] = test_probability >= threshold

    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    model.save_model(str(output / "xgboost_model.json"))
    frame.to_csv(output / "trade_dataset.csv", index=False)
    test_predictions.to_csv(output / "test_predictions.csv", index=False)
    importance = pd.DataFrame({
        "feature": FEATURE_COLUMNS,
        "importance": model.feature_importances_
    }).sort_values("importance", ascending=False)
    importance.to_csv(output / "feature_importance.csv", index=False)

    accepted = test_predictions["model_accept"]
    accepted_pnl = float(test_predictions.loc[accepted, "net_profit_loss"].sum())
    metadata = {
        "schema_version": 1,
        "model_role": "candidate_accept_reject_only",
        "feature_columns": FEATURE_COLUMNS,
        "probability_threshold": threshold,
        "training_rows": len(train),
        "validation_rows": len(validation),
        "test_rows": len(test),
        "sessions": split_sessions,
        "validation_metrics": classification_metrics(
            y_validation.to_numpy(), validation_probability, threshold),
        "test_metrics": classification_metrics(
            test["label_profitable"].astype(int).to_numpy(),
            test_probability, threshold),
        "test_model_accepted_trades": int(accepted.sum()),
        "test_model_accepted_net_pnl": accepted_pnl,
        "dataset_sha256": hashlib.sha256(
            frame.to_csv(index=False).encode("utf-8")).hexdigest(),
        "xgboost_version": xgb.__version__
    }
    with (output / "model_metadata.json").open("w", encoding="utf-8") as stream:
        json.dump(metadata, stream, indent=2, default=str)
    return model, metadata


def load_model(path):
    model = xgb.XGBClassifier()
    model.load_model(str(path))
    return model
