- **ai/ post-ADR-0700 path cleanup**: replaced all stale `libvmaf/build-cpu/tools/vmaf`
  and `libvmaf/src/dnn/` default paths with `core/build-cpu/tools/vmaf` and `core/src/dnn/`
  across `ai/data/`, `ai/src/vmaf_train/`, `ai/scripts/`, and `ai/tests/`.  The `VMAF_BIN`
  env-var override is unaffected.  Fixed a wrong `meson setup core/build-cpu libvmaf` hint
  (source-dir argument was the old directory name; corrected to `core`).  Resolved leftover
  git conflict markers in `ai/src/aiutils/jsonl_utils.py` and `ai/src/vmaf_train/registry.py`
  that were causing import-time `SyntaxError`.  Added missing `--sidecar` / `--out` flags
  and `run_provenance` emission to `ai/lpips_export.py` (fixes `test_lpips_export` failure).
