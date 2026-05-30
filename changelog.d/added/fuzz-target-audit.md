## Added

- **`fuzz_json_model` libFuzzer harness** (`core/test/fuzz/fuzz_json_model.c`):
  wraps `vmaf_read_json_model_from_buffer` and
  `vmaf_read_json_model_collection_from_buffer` (libvmaf SVM model JSON
  parser, `core/src/read_json_model.c`). Seed corpus under
  `json_model_corpus/`; wired into `.github/workflows/fuzz.yml` matrix.
  Closes deferred target #3 ("fuzz_model_load") from
  `docs/research/0083-libfuzzer-harness-expansion-target-survey.md`.
  First run surfaced
  T-JSON-MODEL-SLOPES-FEATURE-CAP-OOB-2026-05-30 — a heap-buffer-overflow
  in `vmaf_model_destroy` when `parse_slopes` outruns `feature_names`;
  reproducer committed under `json_model_known_crashes/` per the ADR-0404
  "keep gates running" policy.
- **`fuzz_dnn_sidecar` libFuzzer harness** (`core/test/fuzz/fuzz_dnn_sidecar.c`):
  wraps `vmaf_dnn_sidecar_load` (tiny-AI sidecar JSON parser,
  `core/src/dnn/model_loader.c`). The loader's `extract_string` /
  `extract_string_array` / `extract_float_array` / `extract_int` helpers
  are hand-rolled `strstr` / `strchr` walkers; the harness writes a
  per-process tempfile pair (`/tmp/vmaf-fuzz-sidecar-<pid>.json`) each
  iteration and exercises the public entry point. Seed corpus under
  `dnn_sidecar_corpus/`; wired into the nightly CI matrix. Closes
  deferred target #4 ("fuzz_sidecar") from Research-0083. 30 s smoke
  ran 3.95 M iterations clean.
