## Added

- **`ai/tests/test_*_unit.py`** — 106 new unit tests across 7 previously
  uncovered AI modules:
  - `ai/data/scores.py` — teacher VMAF score loader (subprocess argv composition,
    NaN handling, env-override paths) — 12 tests.
  - `ai/scripts/eval_probabilistic_proxy.py` — Gaussian / conformal coverage,
    `_z_for_coverage` body of the Acklam approximation, smoke + manifest paths
    under mocked ONNX Runtime — 20 tests.
  - `ai/scripts/fetch_konvid_1k.py` — `_download` / `_extract` / `_humanize` /
    `_archive_record` plus a full mocked end-to-end fetch — 15 tests.
  - `ai/scripts/konvid_to_vmaf_pairs.py` — ffprobe + ffmpeg + libvmaf argv
    composition under mocked subprocess; `_process_clip` cache hit / cache
    write paths — 11 tests.
  - `ai/scripts/measure_quant_drop.py` — `_gate_one` PASS / FAIL / skipped /
    missing-model branches with mocked ONNX Runtime sessions — 12 tests.
  - `ai/scripts/phase3_subset_sweep.py` — SUBSETS registry invariants,
    `_standardize_inplace`, `_summary`, `_loso_sweep` fold cardinality with
    patched `_train_one_fold`, multi-seed reproduction — 14 tests.
  - `ai/scripts/validate_model_registry.py` — structural + cross-file
    consistency invariants (sha mismatch, sidecar presence,
    `quant_mode → int8_sha256`, sigstore_bundle path shape) — 22 tests.
  All external calls (subprocess, urllib, onnxruntime) are mocked so the
  suite runs in <2 s and needs no extra system dependencies.
