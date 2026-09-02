<!-- markdownlint-disable MD060 -->
# ADR-0518: Tiny-model loader accepts external-data and feature-vector ONNX

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude
- **Tags**: `ai`, `dnn`, `loader`, `bug-fix`

## Context

The fork ships three categories of tiny FR regressors under `model/tiny/`:

1. `fr_regressor_v1.onnx` — rank-2 feature-vector model
   (`features` input `[batch, 6]`), with weights stored as external-data
   in a sibling `fr_regressor_v1.onnx.data` file.
2. `fr_regressor_v2.onnx` — rank-2 feature-vector model with **two**
   inputs (`features` `[batch, 6]` + `codec` `[batch, 14]` for the
   encoder-aware variant), also external-data.
3. `vmaf_tiny_v4.onnx` — rank-2 feature-vector model with the
   StandardScaler baked into the graph as Constant nodes (no external
   data, but still rank-2).

Before this change, `vmaf --tiny-model <path>` rejected all three with
`-95` (`ENOTSUP`). The cause was the `in_rank != 4` gate in
`vmaf_ctx_dnn_attach` (libvmaf.c): the C-side bridge had only ever been
wired for NCHW image models like the `dists_sq` checkpoint, so any
rank-2 feature-vector model failed at attach time. ONNX Runtime
**did** load all three successfully — `CreateSession(env, abs_path, …)`
resolves sibling `.onnx.data` files transparently — so the failure was
purely on the libvmaf side, not in ORT.

The trainers (`ai/scripts/train_fr_regressor*.py`) are the source of
truth for the input contract; the loader was the broken party.

## Decision

Extend the tiny-model loader to accept rank-2 feature-vector ONNX models
alongside the legacy rank-4 NCHW path. At per-frame inference time, the
host materialises the feature vector from libvmaf's classic feature
collector — by default the canonical-6
(`adm2`, `vif_scale0..3`, `motion2`) — and feeds it to ORT.

Concrete changes:

- `VmafModelSidecar` gains four new fields (`n_features`,
  `feature_names[]`, `feature_mean[]`, `feature_std[]`,
  `has_feature_scaler`). The sidecar parser accepts both naming
  conventions in use across the trainers: `feature_order` /
  `feature_mean` / `feature_std` (v1 / v2) and `features` /
  `input_mean` / `input_std` (vmaf_tiny_v*).
- `vmaf_ctx_dnn_attach` now branches on `in_rank`:
  - rank 4 → existing NCHW image path.
  - rank 2 → new feature-vector path; allocates a feature scratch
    buffer, discovers any optional second-input width via
    `vmaf_ort_input_shape_at(sess, 1, …)`, allocates and pre-seeds the
    codec block (third-from-last slot set to 1.0 = "unknown" encoder
    one-hot), and records the per-frame dispatch state.
  - other ranks → loud `-ENOTSUP` plus a human-readable log line
    naming the actual rank, so the failure mode is observable.
- `vmaf_ctx_dnn_run_frame` dispatches on the recorded rank. The
  feature-vector path reads each canonical-6 score from
  `vmaf_feature_collector_get_score`, applies the sidecar's
  StandardScaler `(x - mean) / std` when present, packs the tensor,
  and runs single-input or multi-input ORT inference accordingly.
- A new `vmaf_ort_input_shape_at(sess, slot, …)` helper exposes the
  per-slot input shape so the loader can size the optional codec
  block.

ORT external-data handling needs no explicit wiring: passing the
absolute `.onnx` path to `OrtCreateSession` already resolves the
sibling `.onnx.data`. The PR's verification step confirmed this with a
direct ORT-API probe before the libvmaf-side fix landed.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `AddExternalInitializersFromFilesInMemory` plumbing | Explicit control over external-data | Unnecessary — ORT auto-resolves siblings from `CreateSession(abs_path, …)` | Adds code for no observable behaviour change |
| Reject rank-2 with a sharper error and force callers to use a Python wrapper | Smaller patch | Defeats the entire `vmaf --tiny-model` UX for the shipped fr_regressor models | Three of the three production tiny models would remain unusable |
| Require callers to supply the codec block via a new public API | More correct for codec-aware inference | Out of scope for the load-fix; would block on user-facing CLI design (`--tiny-codec libx264 --tiny-preset slow --tiny-crf 23`) | Documented as a follow-up; pre-seeding the "unknown" one-hot keeps the load + run gate green today |
| Mirror Python `_row_to_features` verbatim in C (NaN handling, per_frame_features fallback) | Bit-exact parity with the trainer's synthetic-row path | The trainer's two fallback paths exist for legacy / smoke corpora; libvmaf's collector always populates the canonical-6 in production | Avoid carrying training-only scaffolding into the C surface |

## Consequences

- **Positive**:
  - All three shipped tiny FR regressors load and run via
    `vmaf --tiny-model` for the first time.
  - The new sidecar-driven feature schema makes any future tiny-AI
    model with the same canonical-6 (or any subset / superset within
    `VMAF_DNN_MAX_FEATURE_NAMES = 32`) work without extra C-side
    plumbing.
  - The `-ENOTSUP` path now logs the actual rank instead of leaving
    callers staring at a raw errno, closing a UX gap that consumed
    the e2e-test agent's investigation budget on bug cluster v9.
- **Negative**:
  - The codec block for fr_regressor_v2 is pre-seeded to "unknown
    encoder" today; consumers that need codec-aware predictions
    must wait for a dedicated public API to populate it. Numerical
    drift versus the Python reference is therefore expected for v2
    until that API lands.
  - The feature-vector path reads from the feature collector mid-flight,
    which means motion2's retroactive write (Netflix#910 /
    ADR-0152) lands a frame late — the very first inference sees
    motion2 == 0.0. This is observable but bounded (one frame per
    stream).
- **Neutral / follow-ups**:
  - A future PR can add `--tiny-codec` / `--tiny-preset` / `--tiny-crf`
    CLI flags wired through to `extra_in_buf` so codec-aware models
    get their actual encoder context.
  - The fr_regressor_v2 ensemble seeds and v3 candidates inherit
    this path transparently.

## References

- `req`: user briefing in agent dispatch `Fix vmaf --tiny-model
  ONNX loader so the shipped tiny models actually load. Currently 3
  out of 3 tested tiny models fail with -95.` (2026-05-18).
- `.workingdir/bbb_reports/E2E_TEST_MATRIX_v9.md` items 2d / 2e / 2f —
  e2e diagnostic chain that surfaced the bug class.
- Trainers: `ai/scripts/train_fr_regressor.py`,
  `ai/scripts/train_fr_regressor_v2.py` (source of the
  canonical-6 + codec-block contract).
- Sidecars: `model/tiny/fr_regressor_v1.json`,
  `model/tiny/fr_regressor_v2.json`,
  `model/tiny/vmaf_tiny_v4.json`.
- ONNX Runtime external-data semantics:
  `onnxruntime/python/tools/transformers/large_model_exporter.py`
  upstream comment on sibling-file resolution
  (<https://github.com/microsoft/onnxruntime>).
- Related ADRs: ADR-0040 (multi-input session API),
  ADR-0042 (tiny-AI docs rule), ADR-0249 (fr_regressor_v1 model
  card), ADR-0272 (fr_regressor_v2 model card),
  ADR-0152 (motion2 retroactive write).
