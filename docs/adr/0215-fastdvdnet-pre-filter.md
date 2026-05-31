<!-- markdownlint-disable MD013 MD060 -->
# ADR-0215: FastDVDnet temporal pre-filter — 5-frame window, placeholder weights

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Opus 4.7)
- **Tags**: `ai`, `dnn`, `feature-extractor`, `wave-1`

## Context

Wave 1 of the tiny-AI roadmap calls out FastDVDnet (Tassano, Delon, Veit
2020) as a temporal denoise pre-filter — a published video CNN with a
5-frame sliding window that denoises noisy / grainy sources before they
reach the encoder. The roadmap row in
[`docs/ai/roadmap.md` §3.3](../ai/roadmap.md) flags it as a bigger lift
than per-frame filters because it has to thread a multi-frame buffer
through libvmaf's per-frame extract loop, but explicitly leaves it
in-scope for Wave 1 (T6-7 in `.workingdir2/BACKLOG.md`).

We need three things in one PR: (1) a working contract on the libvmaf
side that the eventual FFmpeg `vmaf_pre_temporal` filter can plug into,
(2) an ONNX checkpoint that the contract can load, and (3) the standard
tiny-AI deliverables (registry row, sidecar JSON, docs, ADR, smoke
test). Real FastDVDnet weights from
[github.com/m-tassano/fastdvdnet](https://github.com/m-tassano/fastdvdnet)
are MIT-licensed but not pinned to a release tag we can vendor
reproducibly without manual download, and training a fresh checkpoint
from scratch takes hours that are out-of-scope for one PR. So this PR
ships the contract + a smoke-only placeholder ONNX, with the real
weights drop tracked as T6-7b.

## Decision

We will ship FastDVDnet as a registered feature extractor
`fastdvdnet_pre` in `core/src/feature/fastdvdnet_pre.c`, backed by an
ONNX model whose I/O contract is

```text
input  "frames"   : float32 NCHW [1, 5, H, W]   # [t-2, t-1, t, t+1, t+2]
output "denoised" : float32 NCHW [1, 1, H, W]
```

The extractor maintains an internal 5-slot ring buffer, gathers the
current window into the input tensor, runs `vmaf_dnn_session_run`, and
emits a per-frame scalar `fastdvdnet_pre_l1_residual` (mean-abs
difference between the input centre frame and the denoised output) so
the existing per-frame plumbing has something to record. The denoised
frame buffer itself is consumed by the FFmpeg-side filter that T6-7b
will land. Initial PR ships a smoke-only placeholder ONNX
(`model/tiny/fastdvdnet_pre.onnx`, ~6 KB, randomly-initialised
3-layer CNN with the correct input/output shape); T6-7b swaps the
weights against a real FastDVDnet checkpoint.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Placeholder ONNX (chosen)** | Unblocks contract + integration in one PR; T6-7b is a weights-only change. | Not a real denoiser yet; downstream consumers must wait for T6-7b before deriving any actual quality benefit. | Picked. Smoke-only is clearly labelled in the registry (`smoke: true`) and the sidecar JSON. |
| Real upstream weights (m-tassano) | Working denoiser day one. | Upstream repo isn't release-tagged; vendoring the checkpoint reproducibly requires manual download + a license-attribution step we'd want to land in a separate PR with the FFmpeg filter glue. | Deferred to T6-7b; bundle the weights drop with the FFmpeg-side filter that actually consumes them. |
| Train a fresh checkpoint locally | No license/sourcing concern; full control over architecture. | Hours of training + dataset prep; out-of-scope for a single PR. | Not done in this PR; could be revisited if upstream weights become unavailable. |
| Pre-filter as a new "preprocessor" registry slot (not a feature extractor) | Cleaner conceptual separation between scoring and pre-filtering. | New registry shape would need its own discovery + lifecycle plumbing across `feature_extractor.c`, the CLI, and the FFmpeg filter wiring; ~3× the surface area for the same first-PR outcome. | Use the existing extractor slot for now; keep the refactor as a follow-up if the second pre-filter (e.g. learned chroma denoise) lands. |
| 3-frame window instead of 5-frame | Simpler ring buffer, less memory. | Diverges from the published FastDVDnet contract that downstream consumers will expect. | Stick with 5-frame to match the paper. |
| Direct frame-buffer hand-off (skip the score scalar) | Closer to the "pre-filter, not a metric" intent. | Breaks the per-frame plumbing's expectation that every registered extractor appends *something* per index; downstream `feature_collector` code would need a special case. | Emit the scalar residual as a sanity record; the buffer hand-off lives on the FFmpeg side (T6-7b). |
| ORT EP selection (force CPU vs auto) | Predictable inference path; no surprise device fallback. | Defeats the multi-EP design that the rest of the tiny-AI surface inherits from `vmaf_dnn_session_open`. | Use `VMAF_DNN_DEVICE_AUTO` (the default) — same as `lpips_sq`. |

## Consequences

- **Positive**: Wave 1 §3.3 row shipped; the FFmpeg filter T6-7b is now
  a swap-in-the-real-weights plus filter-glue PR. The 5-frame ring +
  edge clamp behaviour is fixed in C and exercised by the unit test
  shape contract.
- **Negative**: The shipped ONNX is not a real denoiser. Anyone running
  `vmaf --feature fastdvdnet_pre` against the placeholder will see a
  near-identity pass-through with a tiny random perturbation. The
  registry entry's `smoke: true` flag and the sidecar's `notes` field
  both call this out; the user-facing doc spells out the path to T6-7b.
- **Neutral / follow-ups**:
  - **T6-7b**: vendor the upstream FastDVDnet weights (or train a
    fork-owned checkpoint), drop them under
    `model/tiny/fastdvdnet_pre.onnx`, flip `smoke: false` in the
    registry, and ship the FFmpeg `vmaf_pre_temporal` filter that
    actually consumes the denoised frame buffer.
  - **AGENTS.md invariant**: the 5-frame-window contract + ring-buffer
    edge-clamp behaviour are now load-bearing; any rebase that touches
    `fastdvdnet_pre.c` must preserve both.
  - **Op allowlist**: the placeholder graph uses only ops already in
    [`core/src/dnn/op_allowlist.c`](../../core/src/dnn/op_allowlist.c)
    (`Conv`, `Relu`, `Slice`, `Mul`, `Add`, `Clip`, `Constant`); the
    real FastDVDnet weights drop will need to verify the allowlist
    still covers the upstream graph's ops before flipping the smoke flag.

## References

- Tassano, Delon, Veit, *FastDVDnet: Towards Real-Time Deep Video
  Denoising Without Flow Estimation*, CVPR 2020.
  [arXiv:1907.01361](https://arxiv.org/abs/1907.01361).
- Reference implementation:
  [github.com/m-tassano/fastdvdnet](https://github.com/m-tassano/fastdvdnet)
  (MIT-licensed PyTorch checkpoint).
- [`docs/ai/roadmap.md` §3.3](../ai/roadmap.md) — Wave 1 row that schedules this work.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI 5-point per-PR doc bar.
- [ADR-0107](0107-tinyai-wave1-scope-expansion.md) — Wave 1 scope.
- [ADR-0168](0168-tinyai-konvid-baselines.md) — baseline tiny-AI checkpoint shape we mirror.
- Source: `req` — backlog row T6-7 in
  `.workingdir2/BACKLOG.md` ("FastDVDnet temporal pre-filter (5-frame
  window). Wave 1 — temporal denoising pre-filter via ORT integration.
  Deferred if Wave 1 is too wide; still in-scope.").
