---
paths:
  - tools/vmaf-tune/src/vmaftune/fast.py
  - tools/vmaf-tune/tests/test_fast*.py
invariant: Fast-path is opt-in; Optuna is optional dependency; probe features normalized to canonical range.
---
<!-- markdownlint-disable MD024 -->
# Fast-path optimization proxy

- **Fast-path is opt-in; grid stays canonical
  ([ADR-0276](../../../docs/adr/0276-vmaf-tune-fast-path.md)).**
  `fast` subcommand under `src/vmaftune/fast.py` accelerates
  *recommendation* use case via proxy + Bayesian + GPU-verify, but
  must never automatically replace Phase A grid path. Grid is
  ground-truth corpus generator that Phase B/C/D consume; removing
  or re-routing it breaks Phase A.5 → Phase A fallback contract for
  proxy-OOD sources. `fast` subcommand surfaces its smoke vs
  production mode in CLI output's `notes` field — keep that
  visibility when extending loop.
- **Fast-path time budgets are enforced by Optuna, not only
  reported.** `fast.fast_recommend(time_budget_s=...)` passes value
  to `study.optimize(timeout=...)`, and emitted `n_trials` field is
  number of completed trials, not requested cap. Preserve that
  distinction so wrappers can tell when budget cut search short.
- **`vmaf-tune fast` CLI exit-code contract is fall-back signal**
  (HP-3, ADR-0276 § Status update 2026-05-08). `_run_fast` in
  `cli.py` exits `0` for in-tolerance recommendation, `2` for
  argument errors, and **`3`** for OOD case where proxy/verify gap
  exceeds `--proxy-tolerance`. `|| vmaf-tune recommend ...`
  fall-back idiom in `docs/usage/vmaf-tune.md` depends on non-zero
  exit when gap exceeds tolerance — do not silently downgrade to
  `0` or print warning instead. CLI is **only** seam that injects
  `sample_extractor` (canonical-6 from probe encode + libvmaf JSON
  parse) and `encode_runner` (verify pass) into
  `fast.fast_recommend`; downstream callers that need to re-use
  wiring import `_build_fast_sample_extractor` /
  `_build_fast_encode_runner` rather than re-implementing them.
  Output schema is same JSON shape `recommend` and `predict` emit
  (single source of truth) plus fast-path-specific `verify_vmaf` /
  `proxy_verify_gap` / `score_backend` fields.
- **Optuna is optional runtime dep.** Importing it at module scope
  outside `src/vmaftune/fast.py` (or its tests) is bug. Core
  install path stays zero-dep so corpus generation works on hosts
  that never run fast path. Lazy-import guard in `fast.py` is only
  correct entry point; tests that exercise `fast.py` use
  `pytest.importorskip("optuna")`.
- **Fast-path proxy invariant
  ([ADR-0304](../../../docs/adr/0304-vmaf-tune-fast-path-prod-wiring.md)).**
  Production proxy is **always** `fr_regressor_v2` (no smoke
  models in production path; ADR-0291 flipped v2 to production).
  Every consumer goes through `vmaftune.proxy.run_proxy(...)` —
  single seam over onnxruntime + 14-D codec block (12-way
  ENCODER_VOCAB v2 one-hot + preset_norm + crf_norm). Do not call
  onnxruntime directly from `fast.py` / `recommend.py` /
  `per_shot.py`; future probabilistic-head / ensemble migrations
  (ADR-0393 follow-up) must land in `proxy.py` so callers see no
  diff. Onnxruntime and numpy stay lazy-imported inside `proxy.py`
  so corpus path on hosts without those deps stays zero-dep.
  **Single** GPU verify pass at `fast_recommend` end is mandatory —
  proxy alone never wins, regardless of how confident proxy looks.
  Verification uses existing `score_backend.select_backend`
  selector (ADR-0299); `verify_vmaf` and `proxy_verify_gap` ride on
  result dict. When gap exceeds configured tolerance, result
  flagged OOD; operator falls back to slow Phase A grid (ADR-0276
  fallback contract). `ENCODER_VOCAB_V2` ordering is frozen by
  ADR-0291; reordering silently invalidates every shipped v2
  inference.
- **Fast-path probe feature extraction and normalisation contract
  (T-VMAFTUNE-FAST-PY-PROBE-BROKEN-2026-08-30).** Python fast path
  (`vmaftune.cli._build_fast_sample_extractor`,
  `vmaftune.fast._build_production_sample_extractor`,
  `vmaftune.proxy.normalise_features`) and Go twin (`pkg/fast`)
  share strict numerical and operational contract:
  1. Probe encodes output container bitstreams (e.g. `.mp4`);
     distorted inputs must be decoded to temporary raw YUV via
     `maybe_decode_distorted` prior to libvmaf execution and
     cleaned up in `finally` block.
  2. Feature extraction parses libvmaf pooled metric keys
     (`integer_adm2`, `integer_vif_scale0..3`, `integer_motion2`),
     falling back to bare metric keys and per-frame averages.
  3. Raw canonical-6 features must be normalised with
     `(x - mean) / std` using `feature_mean` and `feature_std`
     from `model/tiny/fr_regressor_v2.json` before evaluating ONNX
     proxy regressor alongside 14-D codec block.
  4. Any non-zero exit from probe encoding or libvmaf feature
     extraction must raise `RuntimeError` immediately — never
     zero-fill (`[0.0] * 6`).
  Cross-language parity is pinned by `tests/test_fast_parity.py`.
  Its `test_e2e_probe_extraction_parity` runs Python extractor and
  `go test ./pkg/fast -run TestProbePipelineExtractsRealFeatures`
  on same fixture. Asserts identical raw pooled means and
  normalised features within 1e-6 (skips when ffmpeg, vmaf CLI or
  Go toolchain absent) — and by seam tests in
  `tests/test_cli_fast.py`. Changing probe argv, pooled-key lookup,
  scaler, or vocabulary on one side without other breaks that
  test.

- **`fast._build_production_sample_extractor` accepts `backend`
  kwarg (ADR-0498 follow-up #7).** Pass `backend=select_backend(...)`
  so TPE proxy trials score on GPU rather than always defaulting
  to CPU. `_build_prod_predictor` and `fast_recommend` forward
  selected backend automatically; test seams that inject custom
  `sample_extractor` callable are unaffected.
- **TPE objective is lowest-bitrate pick rule (ADR-1562).**
  `fast.objective_value` scores CRF that meets target by its predicted
  bitrate and miss by `UNMET_OBJECTIVE_BASE + shortfall`; Go
  `fast.objectiveValue` is same function and both are pinned by one value
  table (`test_pick_lowest_bitrate.py::test_fast_objective_values`,
  `TestObjectiveValue`). Do not restore `abs(vmaf - target) + 1e-4 * kbps`: it
  returned CRF below target whenever it sat closer to it than
  cheapest passing CRF.
