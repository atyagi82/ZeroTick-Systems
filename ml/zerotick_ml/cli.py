import argparse
import json
import sys
from collections import Counter
from pathlib import Path

from .dataset import build_trade_dataset, load_trading_config
from .events import DataError, load_events, resolve_inputs, session_order
from .optimization import optimize
from .training import load_model, train_xgboost


def load_pipeline(path):
    with Path(path).open("r", encoding="utf-8") as stream:
        return json.load(stream)


def resolve_from_root(root, value):
    path = Path(value)
    return path if path.is_absolute() else root / path


def prepare(args):
    root = Path(args.root).resolve()
    pipeline_path = resolve_from_root(root, args.pipeline_config)
    pipeline = load_pipeline(pipeline_path)
    paths = resolve_inputs(pipeline.get("input_globs", []), root)
    events = load_events(paths)
    trading_path = resolve_from_root(root, pipeline.get(
        "trading_config", "config.json"))
    trading = load_trading_config(trading_path)
    output = resolve_from_root(root, pipeline.get(
        "output_dir", "ml/artifacts/latest"))
    weights = [float(value) for value in pipeline.get(
        "book_level_weights", [1.0, 0.8, 0.6, 0.4, 0.2])]
    if len(weights) != 5 or any(value <= 0 for value in weights):
        raise DataError("book_level_weights must contain five positive weights")
    return root, pipeline, trading, events, output, weights, paths


def validation_report(events, paths):
    counts = Counter(event.get("event_type", "unknown") for event in events)
    sessions = session_order(events)
    return {
        "files": [str(path) for path in paths],
        "events": len(events),
        "sessions": len(sessions),
        "first_event_utc": events[0]["event_time_utc"] if events else None,
        "last_event_utc": events[-1]["event_time_utc"] if events else None,
        "event_type_counts": dict(sorted(counts.items())),
        "depth_snapshots": sum(
            1 for event in events
            if event.get("event_type") == "market_snapshot" and
            len(event.get("market", {}).get("bid_levels", [])) == 5 and
            len(event.get("market", {}).get("ask_levels", [])) == 5)
    }


def run(argv=None):
    parser = argparse.ArgumentParser(
        description="ZeroTick shadow-depth ML pipeline")
    parser.add_argument(
        "command", choices=["validate", "optimize", "train", "all"],
        help="Pipeline phase to run")
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--pipeline-config", default="ml/pipeline_config.json")
    parser.add_argument("--model", default=None,
                        help="Existing xgboost_model.json for model-assisted optimization")
    args = parser.parse_args(argv)
    try:
        root, pipeline, trading, events, output, weights, paths = prepare(args)
        report = validation_report(events, paths)
        print(json.dumps(report, indent=2))
        if args.command == "validate":
            return 0

        if args.command in ("optimize", "all"):
            print("Phase 1: optimizing deterministic depth-replay strategy")
            phase1 = optimize(
                events, trading, pipeline, weights, output / "phase1_strategy")
            print(json.dumps(phase1, indent=2, default=str))

        model = None
        metadata = None
        if args.command in ("train", "all"):
            print("Phase 2: training XGBoost candidate-quality model")
            dataset = build_trade_dataset(events, trading, weights)
            model, metadata = train_xgboost(
                dataset, pipeline, output / "phase2_model")
            print(json.dumps(metadata, indent=2, default=str))

        if args.command == "optimize" and args.model:
            model = load_model(resolve_from_root(root, args.model))
        if args.command == "all" or (args.command == "optimize" and model is not None):
            print("Phase 3: optimizing strategy with model acceptance threshold")
            if model is None:
                raise DataError("Phase 3 requires a trained model")
            phase3 = optimize(
                events, trading, pipeline, weights,
                output / "phase3_model_assisted", model=model)
            print(json.dumps(phase3, indent=2, default=str))
        return 0
    except (DataError, FileNotFoundError, json.JSONDecodeError) as error:
        print("pipeline error: {}".format(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(run())
