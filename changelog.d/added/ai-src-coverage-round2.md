- **Round-2 branch-coverage push for `ai/src/` (round 2).**
  Adds 76 focused branch-coverage tests across six `ai/src/` modules not
  touched by the round-1 push (PR #345). Coverage delta on the `ai/`
  subtree: TOTAL **65% → 69%** (+75 statements covered). Per-module:
  - `aiutils/__init__.py`: **64% → 100%** (lazy `__getattr__` paths).
  - `aiutils/jsonl_utils.py`: **96% → 100%** (list-recursion sanitize branch).
  - `vmaf_train/data/datasets.py`: **58% → 100%** (no prior tests).
  - `vmaf_train/data/feature_dump.py`: **44% → 100%** (subprocess-mocked).
  - `vmaf_train/data/manifest_scan.py`: **94% → 100%** (CSV / scan edges).
  - `vmaf_train/bisect_model_quality.py`: **90% → 99%** (gate / `to_dict`).
  - `vmaf_train/registry.py`: **62% → 82%** (pure-Python paths, no ONNX).
