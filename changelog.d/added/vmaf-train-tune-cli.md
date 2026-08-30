- **ai**: Add the documented `vmaf-train tune` CLI surface. The command
  loads the same YAML config as `vmaf-train fit`, accepts repeatable
  `--param` search-space specs for `model_args`, dispatches trials
  through the existing lazy Optuna helper, and keeps Optuna behind the
  optional `ai[tune]` extra.
