# ZeroTick shadow-live ML pipeline

This directory contains an offline research pipeline. It never places orders,
changes `config.json`, or enables live mode. Recommendations are written under
`ml/artifacts/` for manual review.

## The three phases

1. **Depth replay and strategy optimization** replays schema-v1
   `market_snapshot` events in timestamp order and uses Optuna TPE to search
   strategy configuration values. It produces a suggested configuration and
   chronological train/validation/test replay reports.
2. **XGBoost meta-label training** joins accepted `trade_decision`,
   `entry_filled`, and `position_closed` events by `signal_id`. The model learns
   whether an existing LONG/SHORT candidate is likely to be net profitable; it
   does not choose the side.
3. **Model-assisted optimization** repeats depth replay with the trained model
   as a final accept/reject gate and jointly tunes the model probability
   threshold with strategy parameters.

## Required open-source support

- 64-bit Python 3.10, 3.11, or 3.12 (3.11 recommended)
- NumPy and pandas for data handling
- scikit-learn for chronological metrics and validation utilities
- XGBoost for the candidate-quality model
- Optuna for TPE parameter optimization
- joblib (reserved for compatible future preprocessing artifacts)

No paid ML service, GPU, Jupyter, or C++ XGBoost library is required. CPU
training is the default. A GPU can be enabled later by changing XGBoost's
`tree_method`, but reproducibility should be rechecked.

The supported machine-wide setup is Python 3.11 with dependencies installed
into that interpreter:

```powershell
python -m pip install --upgrade pip
python -m pip install -r ml\requirements.txt
```

The current workstation has Python 3.11.9 and the required packages installed
globally. A new terminal is required after PATH changes.

## Collect and place data

Use `run.mode: "shadow_live"`. Its JSONL output must retain schema version 1,
all five bid/ask levels, decisions, fills, and closes. Copy or point the output
to files matching:

```text
data/shadow/*.jsonl
```

Do not concatenate files by sorting their text. The loader orders events by
`event_time_utc` and keeps session boundaries.

## Commands

Validate files and show event/session counts:

```powershell
python ml\run_pipeline.py validate
```

Run phase 1 only:

```powershell
python ml\run_pipeline.py optimize
```

Run phase 2 only:

```powershell
python ml\run_pipeline.py train
```

Run all three phases:

```powershell
python ml\run_pipeline.py all
```

Run model-assisted optimization with an existing model:

```powershell
python ml\run_pipeline.py optimize `
  --model ml\artifacts\latest\phase2_model\xgboost_model.json
```

Edit `ml/pipeline_config.json` to change input patterns, minimum data checks,
Optuna trials, search ranges, or XGBoost parameters.

## Artifacts

Phase 1 writes:

- `strategy_suggested_config.json`
- `strategy_optimization_report.json`
- `strategy_optuna_trials.csv`

Phase 2 writes:

- `xgboost_model.json`
- `model_metadata.json`
- `trade_dataset.csv`
- `test_predictions.csv`
- `feature_importance.csv`

Phase 3 writes corresponding `model_assisted_*` files. Artifacts include the
chronological session split and the training dataset hash. Generated artifacts
and raw shadow data are ignored by Git.

## Data and safety requirements

- Use at least five complete sessions and 100 closed shadow trades for a smoke
  model; substantially more data is needed before relying on results.
- Keep multiple market conditions. A few profitable sessions are not a useful
  training population.
- Labels are net of the configured estimated charges.
- The model is trained only on completed trades. This creates selection bias;
  collecting causal outcomes for rejected candidates is a future improvement.
- Phase 1 replay approximates queue position and fills from sampled depth. It
  cannot reconstruct events between snapshots.
- Never select parameters from the final test report. Collect new sessions and
  rerun the walk-forward process instead.
- A generated configuration is a research proposal, not authorization to
  enable `run.mode: "live"`.

## Backtest-candle three-phase workflow

Backtesting has a separate pipeline because historical OHLCV has no genuine
five-level depth. The collector stores raw minute candles so feature generation
and labels are reproducible; it never fills missing imbalance with invented
values.

Collect 100 development sessions followed by 100 untouched validation
sessions:

```powershell
.\scripts\collect_backtest_ml_data.ps1 -ForceLogin
```

Use `-Output` for another dataset name. Existing output is protected;
`-Overwrite` must be explicit. The collector writes a `.partial` file and
renames it only after writing a completion record.

```powershell
python ml\run_backtest_pipeline.py validate
python ml\run_backtest_pipeline.py all
```

Settings live in `ml/backtest_pipeline_config.json`. The first 100 sessions are
split internally into development-fit and development-validation portions.
The second 100 are scored only at the end and written to
`ml/artifacts/backtest/final_untouched_validation.json`.

Historical features include directional momentum, 5/10-minute trend,
within-session relative volume, estimated spread, intraday range and range
position, candle body/range, volume, and time of day. XGBoost only accepts or
rejects a side selected by momentum/trend agreement. Barrier outcomes use
future OHLC ranges and conservatively count the stop first if target and stop
are both reachable. This fast research approximation is distinct from the C++
one-second execution report; promising settings still require C++ backtests
and shadow-live validation.
