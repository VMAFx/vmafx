<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src/dnn

Orientation for agents working on the ONNX Runtime integration (tiny-AI
inference layer). Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

The C-side runtime for tiny-AI checkpoints. Sits between the feature
extractors and ONNX Runtime.

```text
dnn/
  dnn_api.c / dnn_ctx.h    # public vmaf_dnn_* surface (opened from feature extractors)
  model_loader.c/.h        # loads model/tiny/registry.json, pins paths, checks sha256
  onnx_scan.c/.h           # wire-format scanner — walks ModelProto for banned ops
  op_allowlist.c/.h        # allowlist of ONNX ops we permit (no Scan, bounded Loop/If)
  ort_backend.c/.h         # thin wrapper over ONNX Runtime C API (session + tensors)
  tensor_io.c/.h           # tensor helpers (luma8, RGB + ImageNet normalisation)
  meson.build
```

Public API: [../../include/libvmaf/dnn.h](../../include/libvmaf/dnn.h). The
feature-extractor side consumes this API; no feature code talks to ONNX
Runtime directly.

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../AGENTS.md)).
- **Trust boundary**: any `.onnx` loaded via `--tiny-model` or the registry
  is untrusted input. `onnx_scan.c` is the gate; `op_allowlist.c` is the
  policy; `model_loader.c` does `realpath` + symlink-escape hardening.
  See [ADR-0039](../../../docs/adr/0039-onnx-runtime-op-walk-registry.md).
- **No skipping the scan**: `CreateSession` must not be called before
  `vmaf_dnn_validate_onnx` returns success.
- **Tensor bindings are named**: multi-input graphs bind by ONNX input name
  when `VmafDnnInput::name != NULL`; positional fallback is for single-input
  legacy paths only. See
  [ADR-0040](../../../docs/adr/0040-dnn-session-multi-input-api.md).
- **ImageNet normalisation lives in the graph**, not in the C helper —
  exporters absorb the inverse transform so the C side feeds tensors from
  the shared `vmaf_tensor_from_rgb_imagenet()` helper unchanged. See
  [ADR-0041](../../../docs/adr/0041-lpips-sq-extractor.md).
- **Every tiny-AI change ships docs** under `docs/ai/` in the same PR. See
  [ADR-0042](../../../docs/adr/0042-tinyai-docs-required-per-pr.md).
- **Tiny-AI extractor template is the dedup contract**
  ([ADR-0250](../../../docs/adr/0250-tiny-ai-extractor-template.md)).
  New tiny-AI feature extractors use the helpers in
  [`tiny_extractor_template.h`](tiny_extractor_template.h)
  (`vmaf_tiny_ai_require_runtime` /
  `vmaf_tiny_ai_resolve_model_path` / `vmaf_tiny_ai_open_session` /
  `vmaf_tiny_ai_yuv8_to_rgb8_planes` /
  `vmaf_tiny_ai_yuv_to_rgb8_planes` /
  `VMAF_TINY_AI_MODEL_PATH_OPTION`).
  Each extractor calls `vmaf_tiny_ai_require_runtime()` after
  pixel-format / bit-depth validation and before model-path probing so
  disabled-DNN builds return the ADR-0374 `-ENOSYS` contract instead
  of a misleading missing-model `-EINVAL`.
  The user-facing log lines (`<name>: no model path …`, `<name>:
  vmaf_dnn_session_open(<path>) failed: <rc>`) are wire-format-stable
  across extractors — downstream tooling greps them. Don't introduce
  per-extractor variants of the path / session-open shape; if the
  contract needs to change, update the helpers in one place. The recipe
  lives in
  [`docs/ai/extractor-template.md`](../../../docs/ai/extractor-template.md).
- **Registry schema is the trust contract** (T6-9 / [ADR-0211](../../../docs/adr/0211-model-registry-sigstore.md)).
  Every entry in [`model/tiny/registry.json`](../../../model/tiny/registry.json)
  must satisfy [`registry.schema.json`](../../../model/tiny/registry.schema.json):
  required `id` / `kind` / `onnx` / `sha256`, plus `license` and
  `sigstore_bundle` for `schema_version: 1` entries. New fields are
  added by extending the schema first, then the registry, then any
  consumers — never the other way around. The `--tiny-model-verify`
  path in `model_loader.c` parses the registry inline (no JSON dep) and
  spawns `cosign` via `posix_spawnp(3p)`; `system(3)` is and stays
  banned.
- **`VMAF_TINY_MODEL_DIR` is the optional path jail**. When the env var
  is set, `model_loader.c` canonicalises the requested ONNX path and
  requires it to sit below the canonicalised jail directory before any
  model stat/read. Missing jail dirs, non-directory jail paths,
  sibling-prefix escapes, and symlink escapes fail closed with
  `-EACCES`; keep the regression cases in
  [`test_model_loader.c`](../../test/dnn/test_model_loader.c) together
  with any loader changes.

## Governing ADRs

- [ADR-0020](../../../docs/adr/0020-tinyai-four-capabilities.md) — the four capabilities.
- [ADR-0022](../../../docs/adr/0022-inference-runtime-onnx.md) — ORT runtime + execution-provider mapping.
- [ADR-0023](../../../docs/adr/0023-tinyai-user-surfaces.md) — CLI / C API / ffmpeg / training surfaces.
- [ADR-0036](../../../docs/adr/0036-tinyai-wave1-scope-expansion.md) — Wave 1 scope (LPIPS, MobileSal, TransNet V2, …).
- [ADR-0039](../../../docs/adr/0039-onnx-runtime-op-walk-registry.md) — op-allowlist walk + registry schema.
- [ADR-0040](../../../docs/adr/0040-dnn-session-multi-input-api.md) — multi-input/output API with named bindings.
- [ADR-0041](../../../docs/adr/0041-lpips-sq-extractor.md) — LPIPS-SqueezeNet extractor + ImageNet-in-graph.
- [ADR-0042](../../../docs/adr/0042-tinyai-docs-required-per-pr.md) — doc-substance rule.
- [ADR-0169](../../../docs/adr/0169-onnx-allowlist-loop-if.md) +
  [ADR-0171](../../../docs/adr/0171-bounded-loop-trip-count.md) —
  `Loop` + `If` admitted with bounded trip-count guard
  (`VMAF_DNN_MAX_LOOP_NODES = 16`); `Scan` stays rejected.
- [ADR-0258](../../../docs/adr/0258-onnx-allowlist-resize.md) —
  `Resize` admitted for U-2-Net / mobilesal / saliency / segmentation
  models. Consumers shipping their own ONNX should keep
  `mode in ("nearest", "linear")` (`cubic` not exercised in-tree).
- [ADR-1089](../../../docs/adr/1089-dnn-onnx-domain-bypass.md) —
  `NodeProto.domain` (field 7) validated in addition to `op_type`;
  only `""` and `"ai.onnx"` are permitted (custom/vendor domains
  rejected). Closes the `(domain, op_type)` tuple bypass.
- [ADR-0207](../../../docs/adr/0207-tinyai-qat-design.md) +
  [ADR-0208](../../../docs/adr/0208-learned-filter-v1-qat-impl.md)
  — QAT pipeline (PyTorch QAT → fp32 ONNX → ORT static-quantize
  bridge for PyTorch 2.11 ONNX-exporter limitations).

## ADR-0518 invariants — tiny-model loader accepts rank-2 + external-data ONNX

- **`vmaf_ctx_dnn_attach` accepts `in_rank == 2` AND `in_rank == 4`**.
  Reverting to a `!= 4` gate breaks every shipped FR regressor (the
  three checkpoints under `model/tiny/fr_regressor_v[12]*.onnx` plus
  `vmaf_tiny_v4`). The rank-2 branch lives in
  `dnn_attach_feature_vector()` (file-static helper in
  `libvmaf.c`); the dispatch in `vmaf_ctx_dnn_run_frame` reads
  `vmaf->dnn.in_rank` to route to the NCHW vs feature-vector path.
- **Sidecar parser MUST accept both naming conventions** for the
  feature schema: `feature_order` / `feature_mean` / `feature_std`
  (the trainer style used by `ai/scripts/train_fr_regressor*.py`)
  AND `features` / `input_mean` / `input_std` (the trainer style
  used by `ai/scripts/train_vmaf_tiny_v*.py`). Removing either
  alias breaks one of the two trainer paths silently — the
  loader still loads the ONNX but `n_features` stays 0 and the
  fallback canonical-6 ordering is used unconditionally,
  scrambling models whose feature order differs from
  canonical-6. The `test_sidecar_feature_vector_*` regression
  tests in `test_model_loader.c` gate this.
- **`VMAF_DNN_MAX_FEATURE_NAMES = 32`** is the static cap on the
  in-struct `feature_names[]` / `feature_mean[]` / `feature_std[]`
  arrays. The cap exists to keep `VmafModelSidecar` heap-free
  (Power-of-10 / no-VLA). Increasing it has no behavioural cost
  but lower-bounds the per-context memory; do not shrink it
  below 6 (canonical-6).
- **Oversized sidecars are rejected before stdio reads.**
  `vmaf_dnn_sidecar_load()` performs a `stat()` size check before
  `fopen()` / `fseek()` / `ftell()`. Keep that metadata-only guard:
  the oversized-sidecar regression expects `-EFBIG` without entering
  the normal JSON read path.
- **Pre-seeded "unknown" codec one-hot** in
  `dnn_attach_feature_vector`: when a rank-2 model declares a
  second input, the scratch buffer's third-from-last slot is set
  to 1.0. The "third-from-last" rule mirrors the v2 layout
  (`[encoder_onehot…, preset_norm, crf_norm]`) — the
  "unknown" one-hot lives at index `N-3`. Any future trainer
  that ships a different second-input layout (e.g. inserts a
  new normalised feature between the one-hot and `preset_norm`)
  must keep the "unknown" slot reachable by this offset OR
  update the loader to honour an explicit sidecar
  `unknown_encoder_index` field.
- **ORT external-data resolution is implicit**. Do not add
  `AddExternalInitializersFromFilesInMemory` plumbing —
  `OrtCreateSession(env, abs_path, opts, &session)` already
  resolves sibling `.onnx.data` files. Adding manual external-data
  wiring opens a second code path that drifts.

## Rebase-sensitive invariants (DNN-side surfaces in flight)

- **`ort_backend_internal.h` elem-type accessors mirror ONNX enum values**
  (`VmafOrtElemType UNDEFINED=0 / FLOAT=1 / FLOAT16=10`): numeric values
  are deliberately identical to `ONNXTensorElementDataType` so cast
  comparisons in `ort_backend.c` are safe. Both the `VMAF_HAVE_DNN` path
  (reads `sess->input/output_elem_types[slot]`) and the `!VMAF_HAVE_DNN`
  stub (returns `ELEM_TYPE_UNDEFINED`) must provide `vmaf_ort_internal_input_elem_type`
  and `vmaf_ort_internal_output_elem_type`; removing either breaks the
  `test_ort_internals.c` link on no-ORT builds and blocks the Netflix CPU
  Golden Tests (D24) CI job at the build step. See PR
  `fix/dnn-ort-internals-missing-elem-type-accessors` (2026-06-03).
- **`f16_to_f32_one` subnormal path uses `int32_t exp_adj`, not
  `uint32_t exp`** (fork-local, round-5 `-fsanitize=integer` sweep,
  PR fix/picture-align-unsigned-narrowing): the normalisation loop in
  `tensor_io.c:f16_to_f32_one` iterates a local `int32_t exp_adj = 1`
  counter that is bounded to `[-9, 1]` (10-bit f16 mantissa). An
  earlier implementation used the `uint32_t exp` variable from the
  outer scope, which wrapped through `UINT32_MAX` twice to produce the
  correct f32 biased exponent by modular arithmetic — functionally
  correct but trips `-fsanitize=integer`. Do not revert to the `uint32_t`
  wrap idiom. The `test_f16_to_f32_subnormal` test asserts the exact
  bit-pattern for `0x0001` (smallest positive f16 subnormal, value
  `2^-24`) to catch any accidental regression. See
  [docs/rebase-notes.md](../../../../docs/rebase-notes.md)
  §PR-fix-picture-align-unsigned-narrowing.
- **CoreML EP wiring (ADR-0365, this PR)** — `VmafDnnDevice`
  values 5..8 (`COREML`, `COREML_ANE`, `COREML_GPU`, `COREML_CPU`)
  and the `--tiny-device=coreml{,-ane,-gpu,-cpu}` CLI keywords are
  append-only. The wiring uses the generic
  `SessionOptionsAppendExecutionProvider("CoreMLExecutionProvider", …)`
  form deliberately so the Linux build needs no `coreml_provider_factory.h`
  conditional include; if a future change switches to the typed
  factory, also add a `#if defined(__APPLE__)` guard around the
  include and the call site. The `MLComputeUnits` key string values
  (`CPUAndNeuralEngine` / `CPUAndGPU` / `CPUOnly`) are part of the
  CoreML EP public contract — do not mutate them.
- **CoreML EP coexists with OpenVINO NPU EP (ADR-0332, draft PR
  \#496)**: both ADRs touch the same enum, switch, and CLI grammar
  files. On rebase against either ADR's branch, the conflicts are
  mechanical (adjacent enum values, adjacent switch cases, adjacent
  keyword strings). Keep the enum values in append-only order
  (OpenVINO NPU/_CPU/_GPU = 5..7; CoreML = 5..8 — collision at 5..7
  is resolved by whichever branch lands first taking 5..7 and the
  other taking 8..11). The OpenVINO + CoreML AUTO-chain ordering
  (CUDA → OpenVINO-GPU → ROCm → CoreML → CPU) is
  ADR-0365-Decision-load-bearing.

- **Domain check is load-bearing (ADR-1089)**: `onnx_scan.c` now gates the
  full `(domain, op_type)` tuple, not op_type alone. `read_domain()` rejects
  any `NodeProto.domain` that is neither `""` nor `"ai.onnx"`. Do NOT remove
  this check or widen the allowed-domain set without a new ADR: ORT dispatches
  via `(domain, op_type)` and a non-standard domain can shadow an allowlisted
  op_type with arbitrary custom-op code. If a future consumer requires ONNX-ML
  ops (`"ai.onnx.ml"`) a separate ADR must audit the full ONNX-ML op set and
  justify the expansion.

- **Op-allowlist additions for TransNet V2 (ADR-0257)**:
  `BitShift`, `GatherND`, `Pad`, `Reciprocal`, `ReduceProd`,
  and `ScatterND` are now load-bearing for
  `model/tiny/transnet_v2.onnx` (the upstream ColorHistograms +
  FrameSimilarity branches require all six). On rebase: removing
  any of them from `op_allowlist.c` is a model-breakage event;
  keep the trailing block above the `Loop` / `If` control-flow
  block intact. Future tiny-AI models that want to leverage
  these ops inherit them transparently.
- **Model registry + Sigstore (T6-9, PR #199 open, ADR-0211
  placeholder)**: `--tiny-model-verify` flag wires through to
  `cosign verify-blob` against the Sigstore bundle declared in
  the registry. Pairs with the `quant_mode` / `int8_sha256`
  fields from
  [ADR-0173](../../../docs/adr/0173-ptq-int8-audit-impl.md) /
  [ADR-0174](../../../docs/adr/0174-first-model-quantisation.md).
  On merge: every shipped tiny-AI model needs a Sigstore bundle
  path in `model/tiny/registry.json`.
- **MobileSal (T6-2a, PR #208 open, ADR-0218 placeholder)** —
  saliency feature extractor; opens session via `vmaf_dnn_*`.
- **TransNet V2 (T6-3a + real weights, ADR-0223 + ADR-0261)** —
  shot-boundary detector with real upstream weights; uses the
  bounded-Loop guard from ADR-0171.
- **FastDVDnet (T6-7 / T6-7b, ADR-0215 + ADR-0255)** —
  5-frame window pre-filter; same DNN session contract.
- **OpenVINO NPU EP wiring (ADR-0332, 2026-05-08)** — the
  `VmafDnnDevice` enum carries three explicit OpenVINO selectors
  (`OPENVINO_NPU` / `_CPU` / `_GPU`, values `5..7`) on top of the
  generic `OPENVINO` (value `3`, GPU→CPU fallback chain). The
  explicit-selector branches in `ort_backend.c::vmaf_ort_open` pin
  `try_append_openvino()`'s `device_type` to `NPU` / `CPU` / `GPU`
  with **no** fallback inside the branch — the two-stage CreateSession
  fallback to the CPU EP is shared across all explicit-EP selectors and
  remains the only safety net when the requested OpenVINO device isn't
  present. NPU is intentionally NOT in the AUTO try-chain; opt-in only.
  The `vmaf_dnn_session_attached_ep()` stable-string list gained
  `"OpenVINO:NPU"` — consumers asserting on the returned string
  (documented in `docs/ai/inference.md` §Graceful EP fallback)
  must accept the new value. End-to-end NPU silicon validation is
  deferred to a contributor with Meteor / Lunar / Arrow Lake hardware.

## Invariant — `PRESET_ORDINAL` mirrors Python trainer (ADR-0519)

`model_loader.c::codec_block_preset_ordinal()` is a verbatim port of
`ai/scripts/train_fr_regressor_v2.py::PRESET_ORDINAL` (lines
169..234). When the trainer adds an encoder (e.g. AMD AMF in the
ADR-0302 v3 retrain) or changes a preset ordinal, the C-side table
must update in the same PR — otherwise the codec block populated by
`--tiny-codec` will diverge from what the model was trained against.

The `PRESET_MAX_ORDINAL = 9.0` and `CRF_MAX = 63.0` constants are
shared invariants between the two files; they appear inline in the C
helper rather than as named constants so a `grep '/ 9.0f'` /
`'/ 63.0f'` finds them.

The encoder vocabulary itself comes from the sidecar's
`encoder_vocab` array (loaded into `VmafModelSidecar.encoder_vocab[]`),
not from a duplicated C-side constant, so vocab bumps only require a
new sidecar JSON — no C recompile.

## Invariant — symbolic batch dim acceptance (ADR-0524)

`vmaf_ctx_dnn_attach`'s helpers (`dnn_attach_nchw`,
`dnn_attach_feature_vector`, and the optional rank-2 second-input
shape probe) accept `in_shape[0] ∈ {1, -1}` for the batch dimension.
ORT reports symbolic ONNX dims as `-1` via the C API
(`OrtApi::GetDimensions`), and the per-frame inference loop always
emits `shape[0] = 1` on the ORT Run call, so symbolic batch is
folded to 1 at attach time. **Do not** re-tighten the gate to
`!= 1` — that breaks every shipped NR tiny model
(`model/tiny/nr_metric_v1*.onnx`) plus any future trainer that uses
the PyTorch `torch.onnx.export(..., dynamic_axes=…)` default.

A *fixed* batch > 1 is still rejected (no batched-inference
scheduler exists; the per-frame loop feeds one sample per Run
call). Symbolic H/W (rank-4 spatial dims) remain rejected because
the scratch buffer is sized once at attach time; the diagnostic
distinguishes "symbolic H/W" from "C != 1" so the failure mode is
observable. The `test_attach_accepts_symbolic_batch_rank4`
regression in `test_vmaf_use_tiny_model.c` synthesises a minimal
rank-4 ONNX with `dim_param='batch'` and gates against accidental
re-tightening.

## Invariant — NCHW auto-resize default is DISABLED (ADR-0550)

`vmaf_ctx_dnn_run_frame_nchw` supports auto-resampling the luma plane
to the model's expected NCHW input shape when they differ, using the
filter selected by `vmaf->dnn.resize_mode` (0=DISABLED, 1=BILINEAR,
2=NEAREST, 3=BICUBIC). The enum integer layout is shared between the
public `VmafDnnResizeMode` (`core/include/libvmaf/dnn.h`) and the
internal `VmafTinyResize` (`core/src/dnn/tensor_io.h`); the values
**must** stay 0-indexed and aligned across the two enums — the public
setter casts directly without remapping.

- **Default zero-init is DISABLED**: `vmaf_init` does
  `memset(v, 0, sizeof(*v))`, and `VMAF_TINY_RESIZE_DISABLED == 0`,
  so any context that never calls `vmaf_dnn_set_resize_mode` gets
  the strict -ERANGE-on-mismatch behaviour. Renumbering the enums to
  put a different value at 0 would silently change the default for
  every existing caller. The operator must pass `--tiny-resize bilinear`
  (or equivalent) to enable auto-resize.
- **Matched-dims path stays bit-identical to `vmaf_tensor_from_luma`**:
  `vmaf_tensor_from_luma_resize` forwards verbatim to
  `vmaf_tensor_from_luma` when `src_w == dst_w && src_h == dst_h`.
  This keeps the Netflix golden gate unaffected (FR tiny models
  never hit the resize branch — the user-supplied ref/dist pair is
  already at the right dims). Do NOT introduce a per-pixel codepath
  for the matched-dims case.
- **`DISABLED` semantics live in `libvmaf.c`, not in the helper**:
  the per-frame dispatch routes `VMAF_TINY_RESIZE_DISABLED` to
  `-ERANGE` before calling the resize helper. The helper itself
  returns `-EINVAL` when handed `DISABLED` so a programming bug
  surfaces loudly. Keep the gates in both places — pulling either
  gate folds the disabled-mode semantics into a single point that's
  easier to regress.
- **Coordinate convention is half-pixel-centre**:
  `sx = (dx + 0.5) * src_w / dst_w - 0.5` (and analog for `sy`).
  This matches OpenCV `INTER_*` and torchvision
  `Resize(..., antialias=False)`. Out-of-bounds source coords clamp
  via replicate-edge. Changing the convention silently re-trains
  every shipped image-input model against a slightly different
  distribution.

The `test_resize_*` regressions in
[`../../test/dnn/test_tensor_io.c`](../../test/dnn/test_tensor_io.c)
gate the bit-identical-identity / disabled-EINVAL / nearest
floor-coord behaviour.

## Invariant — codec block layout (ADR-0522)

The second input of `fr_regressor_v2` is exactly
`[encoder_onehot(N_VOCAB), preset_norm, crf_norm]`. The runtime
guards check `extra_in_width == n_encoder_vocab + 2u` at attach time
*and* in the `vmaf_ctx_dnn_set_codec_context` bridge; both checks
must agree. If a future codec-aware model uses a different layout
(e.g. multi-scale codec mixing), bump the sidecar
`codec_block_layout` array and add a dispatch branch — do NOT silently
extend `vmaf_dnn_codec_block_fill` to a different layout.

## Invariant — attached scalar multi-output naming (ADR-0646)

`vmaf_use_tiny_model()` / `vmaf_ctx_dnn_attach()` preserve the old
single-output collector key exactly: sidecar `name` (or
`vmaf_tiny_model`) without an appended output suffix. Multi-output
attached models route through `vmaf_ort_run()` and publish one feature
collector key per scalar ONNX output. The suffix source order is:

1. sidecar `output_names[]` when the array count equals the ONNX output
   count;
2. the ONNX graph output name;
3. deterministic `output<slot>_<attempt>` fallback after sanitisation or
   duplicate collapse.

The attached path is intentionally scalar-only. Do not flatten vector or
image tensors into feature names during a rebase; that needs a new ADR
because it changes report schema cardinality. Also do not revert the
rank-2 / rank-4 frame runners back to `vmaf_ort_infer()` — that helper
is single-output by construction and would reopen T-DNN-MULTI-OUTPUT.

## Invariant — int8 loader redirect and scaler declaration contract

- **Sidecar `quant_mode` drives the redirect**: `vmaf_use_tiny_model()` in
  `dnn_attach_api.c` mirrors `vmaf_dnn_session_open()` in `dnn_api.c`. When the
  companion sidecar declares `quant_mode != VMAF_QUANT_FP32`, the runtime
  redirects to load sibling `<basename>.int8.onnx` if present and valid; if
  absent or invalid, it gracefully falls back to the fp32 baseline per
  ADR-1032 (`VMAF_LOG_LEVEL_DEBUG`). The fallback has **two** triggers and both
  twins must implement both: the int8 file fails the size cap or the op
  allowlist, *and* `vmaf_ort_open()` fails on the int8 path even though it
  passed those gates (an ONNX Runtime build with no kernel for one of its
  quantised ops — `ConvInteger` is the one seen in practice). The redirect must
  never turn an invocation that worked against the fp32 baseline into a hard
  failure; `core/test/dnn/test_cli.sh` covers this through
  `--tiny-model model/tiny/nr_metric_v1.onnx`.
- **`onnx_has_scaler` must match the graph**: If an int8 model's ONNX graph
  bakes in input normalisation / scaling ops (`Sub`/`Div` or scalar constants),
  its companion sidecar `.json` must declare `"onnx_has_scaler": true` so the
  runtime normalisation is bypassed and double-scaling is prevented.
  Enforced over every `model/tiny/*.int8.onnx` by
  `core/test/dnn/test_registry.sh`, `python/test/model_registry_schema_test.py`,
  and `ai/scripts/validate_model_registry.py`. Measured cost of getting this
  wrong: pooled `vmaf_tiny_model` 16.02 instead of 71.95 on the Netflix src01
  pair (`T-TINY-V3-INT8-SIDECAR-MISSING-ONNX-HAS-SCALER-2026-09-04`).
- **The redirect does not check `int8_sha256`**: the only load-time gates are
  the size cap and the op allowlist, in both `dnn_api.c` and
  `dnn_attach_api.c`. Do not add a digest check to one twin without the other,
  and not at all without an ADR — a digest mismatch is a third outcome that
  ADR-1032's fp32-fallback semantics do not currently define.

## Testing

```bash
meson test -C build --suite=dnn
```

Unit tests live under [../../test/dnn/](../../test/dnn/). CI also runs a
`--tiny-model` smoke gate loading a generated 1KB `smoke_v0.onnx` through
the full loader → scanner → session-open path.
