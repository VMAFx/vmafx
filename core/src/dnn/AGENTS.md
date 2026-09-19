<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src/dnn

Orientation for agents working on ONNX Runtime integration (tiny-AI
inference layer). Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

C-side runtime for tiny-AI checkpoints. Sits between feature
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

Public API: [../../include/libvmaf/dnn.h](../../include/libvmaf/dnn.h).
Feature-extractor side consumes this API; no feature code talks to ONNX
Runtime directly.

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../AGENTS.md)).
- **Trust boundary**: any `.onnx` loaded via `--tiny-model` or registry
  is untrusted input. `onnx_scan.c` = gate; `op_allowlist.c` =
  policy; `model_loader.c` does `realpath` + symlink-escape hardening.
  See [ADR-0039](../../../docs/adr/0039-onnx-runtime-op-walk-registry.md).
- **No skipping scan**: `CreateSession` must not be called before
  `vmaf_dnn_validate_onnx` returns success.
- **Tensor bindings are named**: multi-input graphs bind by ONNX input name
  when `VmafDnnInput::name != NULL`; positional fallback is for single-input
  legacy paths only. See
  [ADR-0040](../../../docs/adr/0040-dnn-session-multi-input-api.md).
- **ImageNet normalisation lives in graph**, not in C helper —
  exporters absorb inverse transform so C side feeds tensors from
  shared `vmaf_tensor_from_rgb_imagenet()` helper unchanged. See
  [ADR-0041](../../../docs/adr/0041-lpips-sq-extractor.md).
- **Every tiny-AI change ships docs** under `docs/ai/` in same PR. See
  [ADR-0042](../../../docs/adr/0042-tinyai-docs-required-per-pr.md).
- **Tiny-AI extractor template is dedup contract**
  ([ADR-0250](../../../docs/adr/0250-tiny-ai-extractor-template.md)).
  New tiny-AI feature extractors use helpers in
  [`tiny_extractor_template.h`](tiny_extractor_template.h)
  (`vmaf_tiny_ai_require_runtime` /
  `vmaf_tiny_ai_resolve_model_path` / `vmaf_tiny_ai_open_session` /
  `vmaf_tiny_ai_yuv8_to_rgb8_planes` /
  `vmaf_tiny_ai_yuv_to_rgb8_planes` /
  `VMAF_TINY_AI_MODEL_PATH_OPTION`).
  Each extractor calls `vmaf_tiny_ai_require_runtime()` after
  pixel-format / bit-depth validation and before model-path probing so
  disabled-DNN builds return ADR-0374 `-ENOSYS` contract instead
  of misleading missing-model `-EINVAL`.
  User-facing log lines (`<name>: no model path …`, `<name>:
  vmaf_dnn_session_open(<path>) failed: <rc>`) are wire-format-stable
  across extractors — downstream tooling greps them. Never introduce
  per-extractor variants of path / session-open shape; if
  contract needs to change, update helpers in one place. Recipe
  lives in
  [`docs/ai/extractor-template.md`](../../../docs/ai/extractor-template.md).
- **Registry schema is trust contract** (T6-9 / [ADR-0211](../../../docs/adr/0211-model-registry-sigstore.md)).
  Every entry in [`model/tiny/registry.json`](../../../model/tiny/registry.json)
  must satisfy [`registry.schema.json`](../../../model/tiny/registry.schema.json):
  required `id` / `kind` / `onnx` / `sha256`, plus `license` and
  `sigstore_bundle` for `schema_version: 1` entries. New fields
  added by extending schema first, then registry, then any
  consumers — never other way around. `--tiny-model-verify`
  path in `model_loader.c` parses registry inline (no JSON dep),
  spawns `cosign` via `posix_spawnp(3p)`; `system(3)` is and stays
  banned.
- **`VMAF_TINY_MODEL_DIR` is optional path jail**. When env var
  set, `model_loader.c` canonicalises requested ONNX path,
  requires it to sit below canonicalised jail directory before any
  model stat/read. Missing jail dirs, non-directory jail paths,
  sibling-prefix escapes, and symlink escapes fail closed with
  `-EACCES`; keep regression cases in
  [`test_model_loader.c`](../../test/dnn/test_model_loader.c) together
  with any loader changes.

## Governing ADRs

- [ADR-0020](../../../docs/adr/0020-tinyai-four-capabilities.md) — four capabilities.
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
  only `""` and `"ai.onnx"` permitted (custom/vendor domains
  rejected). Closes `(domain, op_type)` tuple bypass.
- [ADR-0207](../../../docs/adr/0207-tinyai-qat-design.md) +
  [ADR-0208](../../../docs/adr/0208-learned-filter-v1-qat-impl.md)
  — QAT pipeline (PyTorch QAT → fp32 ONNX → ORT static-quantize
  bridge for PyTorch 2.11 ONNX-exporter limitations).

## ADR-0518 invariants — tiny-model loader accepts rank-2 + external-data ONNX

- **`vmaf_ctx_dnn_attach` accepts `in_rank == 2` AND `in_rank == 4`**.
  Reverting to `!= 4` gate breaks every shipped FR regressor
  (three checkpoints under `model/tiny/fr_regressor_v[12]*.onnx` plus
  `vmaf_tiny_v4`). Rank-2 branch lives in
  `dnn_attach_feature_vector()` (file-static helper in
  `libvmaf.c`); dispatch in `vmaf_ctx_dnn_run_frame` reads
  `vmaf->dnn.in_rank` to route to NCHW vs feature-vector path.
- **Sidecar parser MUST accept both naming conventions** for
  feature schema: `feature_order` / `feature_mean` / `feature_std`
  (trainer style used by `ai/scripts/train_fr_regressor*.py`)
  AND `features` / `input_mean` / `input_std` (trainer style
  used by `ai/scripts/train_vmaf_tiny_v*.py`). Removing
  either alias silently breaks one of two trainer paths.
  Loader still loads ONNX, but `n_features` stays 0 and
  fallback canonical-6 ordering is used unconditionally,
  scrambling models whose feature order differs from
  canonical-6. `test_sidecar_feature_vector_*` regression
  tests in `test_model_loader.c` gate this.
- **`VMAF_DNN_MAX_FEATURE_NAMES = 32`** is static cap on
  in-struct `feature_names[]` / `feature_mean[]` / `feature_std[]`
  arrays. Cap exists to keep `VmafModelSidecar` heap-free
  (Power-of-10 / no-VLA). Increasing it has no behavioural cost
  but lower-bounds per-context memory; never shrink it
  below 6 (canonical-6).
- **Oversized sidecars are rejected before stdio reads.**
  `vmaf_dnn_sidecar_load()` performs `stat()` size check before
  `fopen()` / `fseek()` / `ftell()`. Keep that metadata-only guard:
  oversized-sidecar regression expects `-EFBIG` without entering
  normal JSON read path.
- **Pre-seeded "unknown" codec one-hot** in
  `dnn_attach_feature_vector`: when rank-2 model declares
  second input, scratch buffer's third-from-last slot set
  to 1.0. "Third-from-last" rule mirrors v2 layout
  (`[encoder_onehot…, preset_norm, crf_norm]`) —
  "unknown" one-hot lives at index `N-3`. Any future trainer
  shipping different second-input layout (e.g. inserts
  new normalised feature between one-hot and `preset_norm`)
  must keep "unknown" slot reachable by this offset OR
  update loader to honour explicit sidecar
  `unknown_encoder_index` field.
- **ORT external-data resolution is implicit**. Never add
  `AddExternalInitializersFromFilesInMemory` plumbing —
  `OrtCreateSession(env, abs_path, opts, &session)` already
  resolves sibling `.onnx.data` files. Adding manual external-data
  wiring opens second code path that drifts.

## Rebase-sensitive invariants (DNN-side surfaces in flight)

- **`ort_backend_internal.h` elem-type accessors mirror ONNX enum values**
  (`VmafOrtElemType UNDEFINED=0 / FLOAT=1 / FLOAT16=10`): numeric values
  deliberately identical to `ONNXTensorElementDataType` so cast
  comparisons in `ort_backend.c` are safe. Both `VMAF_HAVE_DNN` path
  (reads `sess->input/output_elem_types[slot]`) and `!VMAF_HAVE_DNN`
  stub (returns `ELEM_TYPE_UNDEFINED`) must provide `vmaf_ort_internal_input_elem_type`
  and `vmaf_ort_internal_output_elem_type`; removing either breaks
  `test_ort_internals.c` link on no-ORT builds, blocks Netflix CPU
  Golden Tests (D24) CI job at build step. See PR
  `fix/dnn-ort-internals-missing-elem-type-accessors` (2026-06-03).
- **`f16_to_f32_one` subnormal path uses `int32_t exp_adj`, not
  `uint32_t exp`** (fork-local, round-5 `-fsanitize=integer` sweep,
  PR fix/picture-align-unsigned-narrowing): normalisation loop in
  `tensor_io.c:f16_to_f32_one` iterates local `int32_t exp_adj = 1`
  counter bounded to `[-9, 1]` (10-bit f16 mantissa). Earlier
  implementation used `uint32_t exp` variable from outer scope,
  wrapped through `UINT32_MAX` twice to produce
  correct f32 biased exponent by modular arithmetic — functionally
  correct but trips `-fsanitize=integer`. Never revert to `uint32_t`
  wrap idiom. `test_f16_to_f32_subnormal` test asserts exact
  bit-pattern for `0x0001` (smallest positive f16 subnormal, value
  `2^-24`) to catch any accidental regression. See
  [docs/rebase-notes.md](../../../../docs/rebase-notes.md)
  §PR-fix-picture-align-unsigned-narrowing.
- **CoreML EP wiring (ADR-0365, this PR)** — `VmafDnnDevice`
  values 5..8 (`COREML`, `COREML_ANE`, `COREML_GPU`, `COREML_CPU`)
  and `--tiny-device=coreml{,-ane,-gpu,-cpu}` CLI keywords are
  append-only. Wiring uses generic
  `SessionOptionsAppendExecutionProvider("CoreMLExecutionProvider", …)`
  form deliberately so Linux build needs no `coreml_provider_factory.h`
  conditional include; if future change switches to typed
  factory, also add `#if defined(__APPLE__)` guard around
  include and call site. `MLComputeUnits` key string values
  (`CPUAndNeuralEngine` / `CPUAndGPU` / `CPUOnly`) are part of
  CoreML EP public contract — never mutate them.
- **CoreML EP coexists with OpenVINO NPU EP (ADR-0332, draft PR
  \#496)**: both ADRs touch same enum, switch, and CLI grammar
  files. On rebase against either ADR's branch, conflicts are
  mechanical (adjacent enum values, adjacent switch cases, adjacent
  keyword strings). Keep enum values in append-only order
  (OpenVINO NPU/_CPU/_GPU = 5..7; CoreML = 5..8 — collision at 5..7
  resolved by whichever branch lands first taking 5..7, other
  taking 8..11). OpenVINO + CoreML AUTO-chain ordering
  (CUDA → OpenVINO-GPU → ROCm → CoreML → CPU) =
  ADR-0365-Decision-load-bearing.

- **Domain check is load-bearing (ADR-1089)**: `onnx_scan.c` now gates
  full `(domain, op_type)` tuple, not op_type alone. `read_domain()` rejects
  any `NodeProto.domain` neither `""` nor `"ai.onnx"`. Never remove
  this check or widen allowed-domain set without new ADR: ORT dispatches
  via `(domain, op_type)`, non-standard domain can shadow allowlisted
  op_type with arbitrary custom-op code. If future consumer requires ONNX-ML
  ops (`"ai.onnx.ml"`), separate ADR must audit full ONNX-ML op set,
  justify expansion.

- **Op-allowlist additions for TransNet V2 (ADR-0257)**:
  `BitShift`, `GatherND`, `Pad`, `Reciprocal`, `ReduceProd`,
  and `ScatterND` are now load-bearing for
  `model/tiny/transnet_v2.onnx` (upstream ColorHistograms +
  FrameSimilarity branches require all six). On rebase: removing
  any of them from `op_allowlist.c` = model-breakage event;
  keep trailing block above `Loop` / `If` control-flow
  block intact. Future tiny-AI models leveraging
  these ops inherit them transparently.
- **Model registry + Sigstore (T6-9, PR #199 open, ADR-0211
  placeholder)**: `--tiny-model-verify` flag wires through to
  `cosign verify-blob` against Sigstore bundle declared in
  registry. Pairs with `quant_mode` / `int8_sha256`
  fields from
  [ADR-0173](../../../docs/adr/0173-ptq-int8-audit-impl.md) /
  [ADR-0174](../../../docs/adr/0174-first-model-quantisation.md).
  On merge: every shipped tiny-AI model needs Sigstore bundle
  path in `model/tiny/registry.json`.
- **MobileSal (T6-2a, PR #208 open, ADR-0218 placeholder)** —
  saliency feature extractor; opens session via `vmaf_dnn_*`.
- **TransNet V2 (T6-3a + real weights, ADR-0223 + ADR-0261)** —
  shot-boundary detector with real upstream weights; uses
  bounded-Loop guard from ADR-0171.
- **FastDVDnet (T6-7 / T6-7b, ADR-0215 + ADR-0255)** —
  5-frame window pre-filter; same DNN session contract.
- **OpenVINO NPU EP wiring (ADR-0332, 2026-05-08)** —
  `VmafDnnDevice` enum carries three explicit OpenVINO selectors
  (`OPENVINO_NPU` / `_CPU` / `_GPU`, values `5..7`) on top of
  generic `OPENVINO` (value `3`, GPU→CPU fallback chain).
  Explicit-selector branches in `ort_backend.c::vmaf_ort_open` pin
  `try_append_openvino()`'s `device_type` to `NPU` / `CPU` / `GPU`
  with **no** fallback inside branch. Two-stage CreateSession
  fallback to CPU EP is shared across all explicit-EP selectors,
  remains only safety net when requested OpenVINO device isn't
  present. NPU intentionally NOT in AUTO try-chain; opt-in only.
  `vmaf_dnn_session_attached_ep()` stable-string list gained
  `"OpenVINO:NPU"` — consumers asserting on returned string
  (documented in `docs/ai/inference.md` §Graceful EP fallback)
  must accept new value. End-to-end NPU silicon validation is
  deferred to contributor with Meteor / Lunar / Arrow Lake hardware.

## Invariant — `PRESET_ORDINAL` mirrors Python trainer (ADR-0519)

`model_loader.c::codec_block_preset_ordinal()` is verbatim port of
`ai/scripts/train_fr_regressor_v2.py::PRESET_ORDINAL` (lines
169..234). When trainer adds encoder (e.g. AMD AMF in
ADR-0302 v3 retrain) or changes preset ordinal, C-side table
must update in same PR. Otherwise codec block populated by
`--tiny-codec` diverges from what model was trained against.

`PRESET_MAX_ORDINAL = 9.0` and `CRF_MAX = 63.0` constants are
shared invariants between two files; they appear inline in C
helper rather than as named constants so `grep '/ 9.0f'` /
`'/ 63.0f'` finds them.

Encoder vocabulary itself comes from sidecar's
`encoder_vocab` array (loaded into `VmafModelSidecar.encoder_vocab[]`),
not from duplicated C-side constant, so vocab bumps only require
new sidecar JSON — no C recompile.

## Invariant — symbolic batch dim acceptance (ADR-0524)

`vmaf_ctx_dnn_attach`'s helpers (`dnn_attach_nchw`,
`dnn_attach_feature_vector`, and optional rank-2 second-input
shape probe) accept `in_shape[0] ∈ {1, -1}` for batch dimension.
ORT reports symbolic ONNX dims as `-1` via C API
(`OrtApi::GetDimensions`). Per-frame inference loop always
emits `shape[0] = 1` on ORT Run call, so symbolic batch is
folded to 1 at attach time. **Never** re-tighten gate to
`!= 1` — that breaks every shipped NR tiny model
(`model/tiny/nr_metric_v1*.onnx`) plus any future trainer using
PyTorch `torch.onnx.export(..., dynamic_axes=…)` default.

A *fixed* batch > 1 is still rejected (no batched-inference
scheduler exists; per-frame loop feeds one sample per Run
call). Symbolic H/W (rank-4 spatial dims) remain rejected because
scratch buffer is sized once at attach time; diagnostic
distinguishes "symbolic H/W" from "C != 1" so failure mode is
observable. `test_attach_accepts_symbolic_batch_rank4`
regression in `test_vmaf_use_tiny_model.c` synthesises minimal
rank-4 ONNX with `dim_param='batch'`, gates against accidental
re-tightening.

## Invariant — NCHW auto-resize default is DISABLED (ADR-0550)

`vmaf_ctx_dnn_run_frame_nchw` supports auto-resampling luma plane
to model's expected NCHW input shape when they differ, using
filter selected by `vmaf->dnn.resize_mode` (0=DISABLED, 1=BILINEAR,
2=NEAREST, 3=BICUBIC). Enum integer layout is shared between
public `VmafDnnResizeMode` (`core/include/libvmaf/dnn.h`) and
internal `VmafTinyResize` (`core/src/dnn/tensor_io.h`); values
**must** stay 0-indexed and aligned across two enums — public
setter casts directly without remapping.

- **Default zero-init is DISABLED**: `vmaf_init` does
  `memset(v, 0, sizeof(*v))`, and `VMAF_TINY_RESIZE_DISABLED == 0`,
  so any context never calling `vmaf_dnn_set_resize_mode` gets
  strict -ERANGE-on-mismatch behaviour. Renumbering enums to
  put different value at 0 would silently change default for
  every existing caller. Operator must pass `--tiny-resize bilinear`
  (or equivalent) to enable auto-resize.
- **Matched-dims path stays bit-identical to `vmaf_tensor_from_luma`**:
  `vmaf_tensor_from_luma_resize` forwards verbatim to
  `vmaf_tensor_from_luma` when `src_w == dst_w && src_h == dst_h`.
  This keeps Netflix golden gate unaffected (FR tiny models
  never hit resize branch — user-supplied ref/dist pair is
  already at right dims). Never introduce per-pixel codepath
  for matched-dims case.
- **`DISABLED` semantics live in `libvmaf.c`, not in helper**:
  per-frame dispatch routes `VMAF_TINY_RESIZE_DISABLED` to
  `-ERANGE` before calling resize helper. Helper itself
  returns `-EINVAL` when handed `DISABLED` so programming bug
  surfaces loudly. Keep gates in both places — pulling either
  gate folds disabled-mode semantics into single point that's
  easier to regress.
- **Coordinate convention is half-pixel-centre**:
  `sx = (dx + 0.5) * src_w / dst_w - 0.5` (and analog for `sy`).
  This matches OpenCV `INTER_*` and torchvision
  `Resize(..., antialias=False)`. Out-of-bounds source coords clamp
  via replicate-edge. Changing convention silently re-trains
  every shipped image-input model against slightly different
  distribution.

`test_resize_*` regressions in
[`../../test/dnn/test_tensor_io.c`](../../test/dnn/test_tensor_io.c)
gate bit-identical-identity / disabled-EINVAL / nearest
floor-coord behaviour.

## Invariant — codec block layout (ADR-0522)

Second input of `fr_regressor_v2` is exactly
`[encoder_onehot(N_VOCAB), preset_norm, crf_norm]`. Runtime
guards check `extra_in_width == n_encoder_vocab + 2u` at attach time
*and* in `vmaf_ctx_dnn_set_codec_context` bridge; both checks
must agree. If future codec-aware model uses different layout
(e.g. multi-scale codec mixing), bump sidecar
`codec_block_layout` array, add dispatch branch — never silently
extend `vmaf_dnn_codec_block_fill` to different layout.

## Invariant — attached scalar multi-output naming (ADR-0646)

`vmaf_use_tiny_model()` / `vmaf_ctx_dnn_attach()` preserve old
single-output collector key exactly: sidecar `name` (or
`vmaf_tiny_model`) without appended output suffix. Multi-output
attached models route through `vmaf_ort_run()`, publish one feature
collector key per scalar ONNX output. Suffix source order is:

1. sidecar `output_names[]` when array count equals ONNX output
   count;
2. ONNX graph output name;
3. deterministic `output<slot>_<attempt>` fallback after sanitisation or
   duplicate collapse.

Attached path is intentionally scalar-only. Never flatten vector or
image tensors into feature names during rebase; that needs new ADR
because it changes report schema cardinality. Also never revert
rank-2 / rank-4 frame runners back to `vmaf_ort_infer()` — that helper
is single-output by construction, would reopen T-DNN-MULTI-OUTPUT.

## Invariant — model_loader.c lint shape (ADR-1142 / ADR-0488)

`model_loader.c` is measured by whole-tree clang-tidy ratchet
(`scripts/ci/tidy-baseline-cpu.json`), sits at zero. Three shapes in
it are load-bearing for that; never collapse them on rebase:

- **`parse_sidecar_*()` split.** `vmaf_dnn_sidecar_load()` used to be
  one 202-line / 167-statement / 37-branch function. It is now
  fixed-order driver over `sidecar_json_path()`, `slurp_sidecar_json()`
  and one `parse_sidecar_<field group>()` helper. Call order is
  parse order of original function, is what keeps "later
  field wins" behaviour of malformed sidecar identical. Merging
  helper back inline re-opens `readability-function-size`.
- **`str_to_lower()` calls `(tolower)` parenthesised.** glibc's
  `<ctype.h>` defines `tolower` as five-level nested macro, and
  expansion — not loop — is what tripped nesting-depth budget.
  Parenthesised form suppresses expansion, calls library
  function; behaviourally identical. Never "clean up"
  parentheses.
- **Two `NOLINTNEXTLINE(concurrency-mt-unsafe)` on `getenv()`**, in
  `vmaf_dnn_validate_onnx()` (`VMAF_TINY_MODEL_DIR`) and
  `vmaf_dnn_verify_signature()` (`PATH`). These cite ADR-0488
  caller-contract, same posture as `gpu_dispatch_env.cpp` and
  `core/src/mcp/compute_vmaf.c`. `pthread_once` snapshot is
  deliberately **not** used here: tiny-model tests `setenv()`
  `VMAF_TINY_MODEL_DIR` between cases, must observe each value.

## Invariant — every `core/src/dnn/*.c` keeps `NULL` (ADR-1138)

`dnn_api.c`, `dnn_attach_api.c`, `model_loader.c`, `onnx_scan.c`,
`op_allowlist.c` and `ort_backend.c` each carry one file-scoped
`/* NOLINTBEGIN(modernize-use-nullptr) … ADR-1138. */` …
`/* NOLINTEND(modernize-use-nullptr) */` bracket. Bracket is not
cosmetic: `Build — Windows MSVC + CUDA (build only)` is required
status check, compiles these translation units with `cl.exe`.
Its documented `/std:clatest` C23 feature set does not include
`nullptr` keyword. Rewriting `NULL` to `nullptr` here therefore
fails required lane — PR #1192 had to be reverted for exactly
that reason before ADR-1138 was written.

Keep bracket spanning whole file (`NOLINTEND` is last
line), keep ADR citation in comment, add same bracket to
any new `.c` file in this directory rather than using keyword.
`psnr_tools.cpp` and other C++ TUs are unaffected — ADR-0915's
`modernize-use-nullptr` ratchet still applies to them in full.

## Invariant — test_cli.sh DNN probe must be a valid invocation

`core/test/dnn/test_cli.sh` skips (exit 77) when binary has no DNN
support. Probe has to be *otherwise valid* `vmaf` command line:
`configure_tiny_model()` in `core/tools/vmaf.cpp` runs after argument
validation and after inputs are opened, so bare
`vmaf --tiny-model /dev/null` dies on reference-required gate,
never reaches availability check. Probe therefore feeds real
`src01_hrc01` fixture through `--no-reference`. If availability
check ever moves earlier in `vmaf.cpp`, probe may be simplified —
until then, keep full command line.

## Invariant — int8 loader redirect and scaler declaration contract

- **Sidecar `quant_mode` drives redirect**:
  - entry points: `vmaf_use_tiny_model()` (`dnn_attach_api.c`), `vmaf_dnn_session_open()` (`dnn_api.c`).
  - sidecar `quant_mode != VMAF_QUANT_FP32` -> load sibling `<basename>.int8.onnx` when present and valid; else fp32 baseline, logged at `VMAF_LOG_LEVEL_DEBUG` (ADR-1032).
  - trigger 1: int8 file fails size cap or op allowlist -> each entry point's own path resolver.
  - trigger 2: `vmaf_ort_open()` fails on int8 graph that passed those gates (ONNX Runtime build without kernel for quantised op; seen: `ConvInteger`) -> `vmaf_ort_open_with_fallback()` in `ort_backend.c`, only home. First attempt logs its `CreateSession` failure at DEBUG.
  - both entry points open sessions through `vmaf_ort_open_with_fallback()`; never `vmaf_ort_open()` on int8 path directly. Two private copies drifted once: `T-DNN-ATTACH-INT8-REDIRECT-MISSING-2026-09-04`.
  - never turn invocation that works on fp32 baseline into hard failure. Covered by `core/test/dnn/test_cli.sh` (`--tiny-model model/tiny/nr_metric_v1.onnx`).
- **`onnx_has_scaler` must match graph**: If int8 model's ONNX graph
  bakes in input normalisation / scaling ops (`Sub`/`Div` or scalar constants),
  its companion sidecar `.json` must declare `"onnx_has_scaler": true` so
  runtime normalisation is bypassed, double-scaling prevented.
  Enforced over every `model/tiny/*.int8.onnx` by
  `core/test/dnn/test_registry.sh`, `python/test/model_registry_schema_test.py`,
  and `ai/scripts/validate_model_registry.py`. Measured cost of getting this
  wrong: pooled `vmaf_tiny_model` 16.02 instead of 71.95 on Netflix src01
  pair (`T-TINY-V3-INT8-SIDECAR-MISSING-ONNX-HAS-SCALER-2026-09-04`).
- **Redirect does not check `int8_sha256`**: only load-time gates are
  size cap and op allowlist, in both `dnn_api.c` and
  `dnn_attach_api.c`. Never add digest check to one twin without other.
  Not at all without ADR — digest mismatch is third outcome that
  ADR-1032's fp32-fallback semantics do not currently define.

## Testing

```bash
meson test -C build --suite=dnn
```

Unit tests live under [../../test/dnn/](../../test/dnn/). CI also runs
`--tiny-model` smoke gate loading generated 1KB `smoke_v0.onnx` through
full loader → scanner → session-open path.
