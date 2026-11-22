import unittest
import json
from datetime import datetime, timezone
from pathlib import Path
from tempfile import TemporaryDirectory

import pandas as pd

from zerotick_ml.dataset import estimated_charges
from zerotick_ml.features import market_features, weighted_imbalance
from zerotick_ml.replay import apply_parameters
from zerotick_ml.features import FEATURE_COLUMNS
from zerotick_ml.training import train_xgboost
from zerotick_ml.backtest_pipeline import inspect_dataset


class PipelineTests(unittest.TestCase):
    def setUp(self):
        self.market = {
            "last_price": 100.0, "bid_price": 99.95, "ask_price": 100.05,
            "average_price": 99.5, "open": 99.0, "high": 101.0,
            "low": 98.0, "volume": 100000,
            "bid_levels": [{"price": 99.95 - index * 0.05,
                            "quantity": 200 - index * 10}
                           for index in range(5)],
            "ask_levels": [{"price": 100.05 + index * 0.05,
                            "quantity": 100 - index * 5}
                           for index in range(5)]
        }

    def test_weighted_imbalance_is_directional(self):
        value = weighted_imbalance(self.market, [1, .8, .6, .4, .2])
        self.assertGreater(value, 0.0)

    def test_feature_vector_is_complete(self):
        values = market_features(
            self.market, "LONG", datetime(2026, 1, 1, 10, tzinfo=timezone.utc),
            [1, .8, .6, .4, .2],
            {"relative_volume": 1.5, "price_change_percent": .12,
             "trend_change_percent": .06, "spread_percent": .1,
             "intraday_range_percent": 2.0},
            {"entry_limit": 100.1, "target": 100.6, "stop_loss": 99.8})
        self.assertEqual(19, len(values))
        self.assertGreater(values["weighted_imbalance"], 0.0)

    def test_parameter_application_does_not_mutate_base(self):
        base = {"decision": {"target_percent": .5}}
        changed = apply_parameters(base, {"decision.target_percent": .7})
        self.assertEqual(.5, base["decision"]["target_percent"])
        self.assertEqual(.7, changed["decision"]["target_percent"])

    def test_cost_estimate_is_positive(self):
        config = {"backtest": {}}
        self.assertGreater(estimated_charges("LONG", 100, 101, 100, config), 0)

    def test_historical_manifest_preserves_untouched_split(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "history.jsonl"
            events = [
                {"schema_version": 1, "event_type": "historical_dataset_started",
                 "dataset_id": "sample", "training_sessions": ["2026-01-01"],
                 "validation_sessions": ["2026-01-02"]},
                {"schema_version": 1, "event_type": "historical_symbol_session",
                 "dataset_id": "sample", "split": "train",
                 "session_date": "2026-01-01", "symbol": "TEST", "candles": [{}]},
                {"schema_version": 1, "event_type": "historical_symbol_session",
                 "dataset_id": "sample", "split": "validation",
                 "session_date": "2026-01-02", "symbol": "TEST", "candles": [{}]},
                {"schema_version": 1, "event_type": "historical_dataset_completed",
                 "dataset_id": "sample", "records": 2, "candles": 2}
            ]
            path.write_text("".join(json.dumps(item) + "\n" for item in events),
                            encoding="utf-8")
            result = inspect_dataset([path])
            self.assertEqual(["2026-01-01"], result["train_sessions"])
            self.assertEqual(["2026-01-02"], result["validation_sessions"])

    def test_xgboost_training_smoke(self):
        rows = []
        for session in range(6):
            for index in range(10):
                label = (session + index) % 2
                row = {name: float(index + feature_index) / 10.0
                       for feature_index, name in enumerate(FEATURE_COLUMNS)}
                row.update({
                    "session_id": "session-{}".format(session),
                    "label_profitable": label,
                    "net_profit_loss": 10.0 if label else -5.0,
                    "decision_time_utc": datetime(
                        2026, 1, session + 1, tzinfo=timezone.utc)
                })
                rows.append(row)
        frame = pd.DataFrame(rows)
        config = {
            "minimum_sessions": 5,
            "minimum_completed_trades": 30,
            "validation_session_fraction": 0.2,
            "test_session_fraction": 0.2,
            "random_seed": 42,
            "objective": {"minimum_trades": 2},
            "xgboost": {
                "n_estimators": 20, "max_depth": 2,
                "learning_rate": 0.1, "tree_method": "hist",
                "objective": "binary:logistic", "eval_metric": "logloss",
                "early_stopping_rounds": 5
            }
        }
        with TemporaryDirectory() as directory:
            _, metadata = train_xgboost(frame, config, Path(directory))
            self.assertTrue((Path(directory) / "xgboost_model.json").exists())
            self.assertEqual(40, metadata["training_rows"])


if __name__ == "__main__":
    unittest.main()
