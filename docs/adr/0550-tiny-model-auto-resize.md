# ADR-0550: Auto-resize input plane to NR tiny-model dims + `--tiny-resize` flag

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude
- **Tags**: `ai`, `cli`, `dnn`, `api`

## Context

The fork's NR tiny-model dispatch (`vmaf_ctx_dnn_run_frame_nchw` in
`core/src/libvmaf.c`) is the per-frame handler that feeds the
ONNX Runtime session for image-input tiny models (rank-4 NCHW). The
shipped NR scorer `model/tiny/nr_metric_v1.onnx` has a fixed input
shape of `[1, 1, 224, 224]` (KoNViD-1k middle-frame MobileNet trained
at 224x224 grayscale; see `model/tiny/nr_metric_v1.json:notes`).

Before this ADR the dispatch hard-errored with `-ERANGE` on any
dimension mismatch — including the common case of running a YUV
fixture (e.g. `src01_hrc01_576x324.yuv` at 576x324) through the
224x224 model. Because the error bubbled up via
`vmaf_read_pictures` → the CLI loop's break, the user saw **0 frames
scored**, an empty `frames` array in the JSON output, and the
silent "0 frames" footer instead of any actionable feedback. The
post-fix probe (Finding 11) flagged this as the next user-facing
blocker after `fix/auto-select-engages-cuda` (commit `e5d26e238`)
unlocked CUDA auto-select.

The legacy contract was symmetric with the FR (full-reference) tiny
models, where the user supplies a matched pair and the kernel
operates on bit-exact native dims. NR scoring breaks that
assumption: there is no reference to match against, and the model's
training resolution is an internal property of the checkpoint
(visible only in the sidecar `notes` string), not a public part of
the CLI surface. Forcing every NR user to pre-scale their YUV to the
exact model dims is unrealistic — KoNViD-1k inputs vary, the
training resolution differs per checkpoint, and the failure mode is
silent.

## Decision

We make the per-frame NCHW dispatch auto-resize the luma plane to
the model's static input shape when the dims differ, using a
deterministic separable filter selected at runtime. The **default is
`DISABLED`**: a dimension mismatch returns `-ERANGE` (the
pre-ADR-0550 hard-error), preserving the strict mode for parity
harnesses and avoiding a silent free parameter. The operator must
explicitly pass `--tiny-resize {bilinear,nearest,bicubic}` to enable
auto-resize; filter choice must be documented alongside the model
checkpoint because bilinear, nearest, and bicubic produce scores that
differ by approximately 2% on the same input.

The three supported filters are bilinear (matching torchvision
`Resize(..., antialias=False)` and OpenCV `INTER_LINEAR`), nearest
(floor-coord, cheapest), and bicubic (Catmull-Rom a=-0.5, for parity
with `transforms.Resize(interpolation=BICUBIC)` exporters).

When the source dims already equal the model dims, the resize helper
forwards verbatim to `vmaf_tensor_from_luma`, keeping the
matched-dims path bit-identical regardless of the selected filter.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **DISABLED default, explicit opt-in (chosen)** | Preserves the pre-ADR-0550 strict mode for parity harnesses. No silent free parameter: the operator must name the filter and can document it alongside the model checkpoint. Zero-init struct field = DISABLED, so no runtime initialization overhead. | Requires an explicit `--tiny-resize bilinear` for the most common NR workflow; first-time users see -ERANGE until they read the docs. | Lowest-footgun approach: the ~2% score spread across filters means auto-selecting bilinear is a silent bias that downstream consumers cannot detect. DISABLED-as-default makes the choice explicit and auditable. |
| **Auto-resize, default bilinear** | Removes the silent-zero-frames footgun in the most common NR workflow. Matches torchvision / OpenCV training-time convention. | Filter choice is a silent free parameter producing ~2% score spread on the same input. A pipeline that forgets to set `--tiny-resize` gets bilinear silently; a model trained against bicubic will see a small but systematic bias. | Rejected per user direction: bilinear/nearest/bicubic producing ~2% spread is a footgun. DISABLED forces the operator to choose consciously. |
| **Fail fast with a more informative diagnostic** | Zero behavioural change; easy to ship; surfaces the mismatch loudly. | The user still has to pre-scale the YUV externally (ffmpeg `scale=224:224` etc.). Doubles the disk + decode cost. Re-implements at the user level what every PyTorch dataloader does in 1 line. The diagnostic does not actually unblock any workflow. | Rejected: makes the tool usable by experts only. |
| **Require matched YUV (refuse to attach the model unless dims match)** | Fails at model-load time, not at frame 0 — the diagnostic is even louder. | Still requires the user to pre-scale every NR input. Breaks the FR path's symmetry. | Rejected: same usability ceiling as the diagnostic option with extra rigidity. |
| **Insert a `Resize` op into the ONNX graph at model-load time** | The model's static input shape becomes dynamic for free; no C-side resize needed. | Requires a full ONNX graph mutation pass on every load. The fork's `op_allowlist.c` already gates `Resize` (ADR-0258); a runtime-injected op opens an attack surface. Untenable for `.int8.onnx` quantised siblings. | Rejected: solves the same problem at much higher implementation cost and ecosystem risk. |
| **Run the resize inside ONNX Runtime via a `SessionOptions` graph transformer** | Native to ORT; no C-side maintenance burden. | The `SessionOptions::AddTransformer` C API is internal-only and not part of the stable ORT contract. The transformer would still have to know the source dims at first-frame time, not at model-load time. | Rejected: tighter coupling to a non-stable ORT surface for no functional gain. |

## Consequences

- **Positive**:
  - `vmaf --no-reference --tiny-model nr_metric_v1.onnx --tiny-resize bilinear`
    works out-of-the-box against any YUV resolution.
  - The default (DISABLED) makes the filter choice explicit and auditable;
    no silent score bias from an auto-selected filter.
  - The matched-dims fast path is bit-identical to the legacy code
    (forwards verbatim to `vmaf_tensor_from_luma`), so the Netflix golden
    gate is unaffected.
  - Public C API gains a small, focused setter (`vmaf_dnn_set_resize_mode`)
    instead of growing `VmafDnnConfig` with another flag — keeps the
    surface factored.
- **Negative**:
  - One more knob to document and version. The CLI grammar gains a new
    `--tiny-resize` keyword set; any downstream wrapper that pre-parses
    CLI flags must accept it.
  - First-time users see -ERANGE until they add `--tiny-resize bilinear`
    (or equivalent). The error message must be actionable.
- **Neutral / follow-ups**:
  - A future tiny-model sidecar field
    (`expected_resize: "bilinear" | "bicubic" | ...`) could drive the
    default per-model; until that exists, the CLI flag + global default is
    the contract.
  - `docs/ai/inference.md` gains an `## Auto-resize for image tiny models`
    section documenting DISABLED-as-default and the ~2% spread warning.

## References

- `req`: post-fix probe Finding 11 — "Fix the NR-model input-size
  mismatch in VMAFx/vmafx surfaced by the post-fix probe …"
  (paraphrased: NR-path dispatch returns `-ERANGE` when the model's
  expected spatial dims don't match the YUV; the loop breaks at
  frame 0 -> 0 frames scored).
- ADR-0524 — symbolic batch dim acceptance in
  `vmaf_ctx_dnn_attach` (shares the per-frame NCHW dispatch).
- ADR-0258 — `Resize` op admitted to the ONNX allowlist (used by
  U-2-Net / mobilesal / saliency models that resize internally).
- ADR-0042 — tiny-AI docs required per PR (this ADR ships
  `docs/ai/inference.md §Auto-resize`).
- `core/src/dnn/tensor_io.c::vmaf_tensor_from_luma_resize`
  (this PR).
- `core/src/libvmaf.c::vmaf_ctx_dnn_run_frame_nchw` (this PR
  wires the resize through).
- Source: `req` (post-fix probe Finding 11, 2026-05-18).
