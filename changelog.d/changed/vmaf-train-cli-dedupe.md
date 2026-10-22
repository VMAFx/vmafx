- **`vmaf-train fit` and `vmaf-train tune` apply their config overrides in one
  place.** `ai/src/vmaf_train/cli.py` loads the YAML config with the
  `--cache`, `--output`, `--epochs` and `--seed` overrides through one helper,
  and `op_allowlist.check_model()` loads the ONNX file and delegates to
  `check_graph()` (HISS-19). Options and output are unchanged.
