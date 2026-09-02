Made `vmaf-tune fast --time-budget-s` enforce a real Optuna timeout and
report completed trials in the JSON `n_trials` field instead of treating the
flag as advisory metadata.
