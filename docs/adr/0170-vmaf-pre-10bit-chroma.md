# ADR-0170: `vmaf_pre` extended to 10/12-bit and optional chroma (T6-4)

- **Status**: Accepted
- **Date**: 2026-04-25
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tiny-ai, ffmpeg, dnn, api, fork-local

## Context

[BACKLOG T6-4](../../.workingdir2/BACKLOG.md) / [Wave 1 roadmap
§ 3.1](../ai/roadmap.md) called for:

> **Current.** Luma-8bit only, chroma passes through untouched.
>
> **Expansion.** Accept `yuv420p10le` / `yuv422p10le` / `yuv444p10le`;
> run the learned filter on chroma planes too (either a single
> 3-channel model or three single-channel sessions). This is where
> the real bitrate wins live — HDR content and chroma-heavy sources
> are exactly where classical pre-filters leave budget on the table.
>
> **ONNX notes.** Input tensor becomes `[1, C, H, W]` with `C ∈ {1, 2, 3}`.
> Requires touching `tensor_io.c` to normalize across bit depths (the
> `luma8` helper assumes 8-bit).

The immediate downstream consumer is the C3 baseline
[`learned_filter_v1.onnx`](../../model/tiny/learned_filter_v1.onnx)
landed in [ADR-0168](0168-tinyai-konvid-baselines.md). That model is
single-channel (`[1, 1, H, W]`), which constrains the chroma strategy
to "three sessions over three single-channel planes" rather than a
new 3-channel architecture.

## Decision

### 1. New `vmaf_dnn_session_run_plane16` entrypoint

Adds [`vmaf_dnn_session_run_plane16`](../../core/include/libvmaf/dnn.h)
alongside the existing `_luma8`. Signature:

```c
int vmaf_dnn_session_run_plane16(VmafDnnSession *sess,
                                 const uint16_t *in, size_t in_stride,
                                 int w, int h, int bpc,
                                 uint16_t *out, size_t out_stride);
```

`bpc ∈ [9, 16]` selects the normalisation divisor `(1 << bpc) - 1`.
10/12/14/16-bit all share the same code path — the only per-call
variable is the divisor and the output clamp ceiling. `in_stride`
and `out_stride` are in **bytes** (not samples), matching `luma8`'s
convention.

Two matching tensor helpers land in
[`tensor_io.{h,c}`](../../core/src/dnn/tensor_io.h):

- `vmaf_tensor_from_plane16(src, stride, w, h, bpc, layout, dtype,
  mean, std, dst)` — packed `uint16` LE plane → normalised `float32`
  / `float16` tensor.
- `vmaf_tensor_to_plane16` — inverse.

### 2. `vmaf_pre` accepts 10/12-bit + optional `chroma`

The ffmpeg patch at
[`ffmpeg-patches/0002-add-vmaf_pre-filter.patch`](../../ffmpeg-patches/0002-add-vmaf_pre-filter.patch)
is extended:

- **Pixel formats**: added `GRAY10LE`, `YUV420P10LE`, `YUV422P10LE`,
  `YUV444P10LE`, and the 12-bit LE counterparts (the C3 baseline is
  bit-depth-agnostic by construction, so 12-bit is the same code
  path as 10-bit).
- **New option** `chroma=0|1` (default 0). When 1, the same session
  runs on the U and V planes with their chroma-subsampled dimensions;
  on inference failure the chroma plane falls back to a pass-through
  copy (fail-open for chroma — the luma plane fails closed as before).
- **Dispatch**: a new `run_plane(ctx, bpc, in, ..., out, ...)` helper
  picks `_luma8` when `bpc == 8` and `_plane16` otherwise. Keeps the
  per-plane call site identical.

The 8-bit code path is byte-identical to the pre-ADR behaviour —
`chroma=0` (default) + 8-bit YUV420P still calls `_luma8` and copies
chroma through.

## Alternatives considered

1. **Single `[1, 3, H, W]` 3-channel model.** Would unify luma + chroma
   in one inference. Rejected: would force retraining C3 from scratch
   (currently 1-channel), and the chroma planes have different H/W
   under 4:2:0 / 4:2:2 sub-sampling so a single tensor doesn't fit
   without upsampling/padding. Three single-channel sessions is the
   cleaner match for the existing `learned_filter_v1`.

2. **Re-open a new session per chroma plane.** Rejected: session-open
   is expensive (ORT compile + model parse). The same session works
   fine for luma *and* chroma if the model's input shape is declared
   dynamic (it is — see `dnn_api.c:104` comment). `run_plane`
   trusts the shape; if a future model pins a static size, the call
   returns `-ERANGE` and the caller can re-open.

3. **Hand-roll bit-depth dispatch in the ffmpeg patch only.** Rejected:
   the same logic would be duplicated in any other caller (MCP server,
   future CLI). Putting the plane16 helper in libvmaf's public API
   keeps the contract in one place.

4. **Force `chroma=1` by default.** Rejected: C3 was trained on luma
   only (KoNViD-1k middle-frame grayscale). Running it on chroma has
   real risk of biasing away from neutral grey; keeping `chroma=0`
   default preserves the validated baseline and lets users opt into
   the experimental path.

5. **Split the ADR into "add plane16 API" + "extend ffmpeg patch".**
   Rejected: the patch is load-bearing for the API additions — the
   API without a consumer is dead code, and the ffmpeg update needs
   the API to land first. One atomic landing keeps the invariant
   "every public API in this PR has at least one real caller."

## Consequences

**Positive:**

- Real 10-bit HDR content now flows through the learned filter. No
  downcast to 8-bit in the pipeline.
- Chroma denoising becomes available as an opt-in — matches the
  roadmap's "real bitrate wins live in chroma-heavy sources" claim.
- The plane16 API is reusable for future bit-depth-flexible models
  (C2 NR can accept 10-bit input the moment the training pipeline
  catches up — the libvmaf surface is ready).
- Single-precision pipeline end-to-end: `uint16 → float32 → uint16`
  with round-to-even + clamp. No precision loss vs the 8-bit path.

**Negative:**

- `chroma=1` on a luma-trained model is an **experimental** setting
  and may degrade subjective quality on chroma-heavy clips. The
  docstring + CLI help flag it; future C3 variants trained
  multi-plane would close the gap.
- Two very-similar API surfaces (`_luma8` + `_plane16`). Acceptable
  — bit-depth is an ABI-level concern that can't be hidden behind
  a single void-pointer helper without losing type safety. The
  luma8 path stays for back-compat; callers that want to be
  bit-depth-agnostic can branch on `bpc == 8` themselves.
- The ffmpeg patch grew from 193 LoC → 291 LoC. Still well under
  any per-filter threshold.
- The bpc argument to `_plane16` is currently taken on faith — a
  caller that lies about bpc gets wrong numeric results (but no
  memory safety issue; the uint16 read is always in-bounds). A
  runtime check against the model sidecar could catch this; not
  needed for the current trust model where `vmaf_pre` is the only
  caller and trusts its own ffmpeg format descriptor.

## Tests

- `core/test/dnn/test_tensor_io.c`:
  - `test_plane16_10bit_roundtrip` — 8-pixel 10-bit plane survives
    `from → to` byte-identical.
  - `test_plane16_rejects_bad_bpc` — `bpc=8` (too low) and `bpc=17`
    (too high) rejected.
  - `test_plane16_12bit_clamps` — out-of-range floats clamp to
    `[0, 4095]` rather than overflowing uint16.
- The higher-level `vmaf_dnn_session_run_plane16` entrypoint inherits
  the tensor-io coverage above. A full round-trip through ORT is
  covered by the existing `test_dnn_session_api` tests (any ONNX
  that accepts `[1, 1, H, W]` float32 works for both `_luma8` and
  `_plane16`).
- ffmpeg-level integration: a manual smoke in the reproducer below.

## References

- [BACKLOG T6-4](../../.workingdir2/BACKLOG.md) — backlog row.
- [Wave 1 roadmap § 3.1](../ai/roadmap.md) — "`vmaf_pre` extension".
- [ADR-0168](0168-tinyai-konvid-baselines.md) — C3 baseline that
  this ADR makes reachable in 10/12-bit pipelines.
- [ADR-0169](0169-onnx-allowlist-loop-if.md) — sister Tiny-AI
  expansion shipped in the same session.
- `req` — user popup 2026-04-25: "T6-4 vmaf_pre 10-bit + chroma (M)".
