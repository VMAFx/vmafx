<!-- markdownlint-disable MD013 MD060 -->
# ADR-0255: FastDVDnet temporal pre-filter — real upstream weights via luma adapter (T6-7b)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (Opus 4.7)
- **Tags**: `ai`, `dnn`, `feature-extractor`, `wave-1`, `weights-drop`

## Context

[ADR-0215](0215-fastdvdnet-pre-filter.md) shipped the FastDVDnet
temporal pre-filter contract — a registered feature extractor
`fastdvdnet_pre`, a 5-frame ring buffer in
`core/src/feature/fastdvdnet_pre.c`, a smoke-only placeholder ONNX
under `model/tiny/fastdvdnet_pre.onnx`, and the smoke test plumbing.
The placeholder was a randomly-initialised 3-layer CNN matching the
declared I/O shape (`frames` `[1, 5, H, W]` luma, `denoised`
`[1, 1, H, W]`); the registry row carried `smoke: true` and the doc
flagged the missing real weights as backlog item T6-7b.

Two structural mismatches blocked a verbatim weights drop:

1. **RGB vs luma.** Upstream FastDVDnet (Tassano, Delon, Veit 2020;
   `github.com/m-tassano/fastdvdnet`, MIT) takes RGB inputs of shape
   `(1, 5*3, H, W) = (1, 15, H, W)` plus a per-pixel noise map of shape
   `(1, 1, H, W)`. The fork's C contract is luma-only with no
   noise-map input.
2. **PixelShuffle.** Upstream's UpBlock uses `nn.PixelShuffle`, which
   PyTorch's ONNX exporter emits as `DepthToSpace`. `DepthToSpace` is
   not on the fork's strict ONNX op allowlist
   (`core/src/dnn/op_allowlist.c`).

Both mismatches need resolving in the same PR or the weights drop
breaks either the C extractor or the model loader's allowlist scan.

## Decision

We will ship the upstream FastDVDnet checkpoint (pinned at commit
`c8fdf61`, sha256
`9d9d8413c33e3d9d961d07c530237befa1197610b9d60602ff42fd77975d2a17`)
wrapped by a small `LumaAdapter` PyTorch module that preserves the
existing C-side I/O contract:

- replicate each luma plane into RGB via `Concat` (`Y -> [Y, Y, Y]`)
  to match upstream's 15-channel input;
- broadcast a constant `sigma = 25/255` noise map (the upstream
  reference inference noise level used by `test_fastdvdnet.py`) via
  `ones_like(centre) * sigma`;
- collapse the upstream RGB output back to luma using BT.601 weights
  `Y = 0.299 R + 0.587 G + 0.114 B`.

We will additionally swap every `nn.PixelShuffle` instance in the
upstream graph for an allowlist-safe `Reshape` → `Transpose` →
`Reshape` decomposition before exporting. PixelShuffle has zero
learned parameters, so the swap is numerically identical (verified
`< 1e-6` max-abs diff between the upstream PyTorch graph and the
exported ONNX on random luma inputs).

The export script lives at `ai/scripts/export_fastdvdnet_pre.py`; it
verifies the upstream weights checksum, rebuilds the wrapped graph,
and re-emits the registry row with `smoke: false`,
`license: "MIT"`, the upstream commit pin, and a refreshed sha256.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Luma adapter + PixelShuffle swap (chosen)** | Real working denoiser; zero changes to the C extractor or its tests; reproducible from a pinned upstream checkpoint; one-PR delivery. | Network was not trained on luma-tiled-into-RGB inputs, so denoising quality is below a luma-native retrain. | Picked. The quality cost is bounded (BT.601 round-trip is well-defined) and the path to a luma retrain (T6-7c) is explicit in the doc. |
| Change the C extractor to accept RGB + noise map | Faithful to upstream training distribution. | Breaks the public extractor contract pinned in ADR-0215; requires a co-ordinated update to the future FFmpeg `vmaf_pre_temporal` filter wiring; doubles the surface area of this PR. | Deferred. If a luma retrain (T6-7c) lands, the contract stays luma-only anyway. |
| Train a luma-native FastDVDnet from scratch | No domain mismatch; smaller checkpoint feasible. | Hours of training + dataset prep; out-of-scope for one PR (same blocker that drove ADR-0215's placeholder). | Tracked as T6-7c. |
| Keep DepthToSpace in the allowlist | Simpler export script (no monkey-patching of upstream's UpBlock). | Widens the trusted op surface for the entire DNN loader; every future model that uses DepthToSpace inherits the change. | Rejected. PixelShuffle is shape-only, so the decomposition costs nothing and keeps the allowlist tight. |
| INT8-quantize the wrapper graph (per-model PTQ, ADR-0174 pattern) | Smaller checkpoint (~2.5 MiB instead of 9.5 MiB). | PTQ requires representative calibration data the fork doesn't have for video denoising; quality budget verification absent. | Deferred. The 9.5 MiB fp32 checkpoint is well under the 50 MiB DNN size cap; PTQ is a follow-up if the size matters. |
| FP16 export (smaller checkpoint, common DNN mode) | Halves the on-disk size. | The fork's DNN loader path defaults to fp32 inference; mixing fp16 weights complicates the smoke-gate path. | Deferred until a fork-wide fp16 inference policy lands. |

## Consequences

- **Positive**: T6-7b row closed; `fastdvdnet_pre` ships a real
  working denoiser whose output now meaningfully tracks the reference
  inference behaviour from upstream's published checkpoint. The
  registry row's `smoke: false` flag and the sidecar JSON's
  upstream-commit pin make the weights provenance auditable.
- **Negative**: 9.5 MiB additional binary content under
  `model/tiny/fastdvdnet_pre.onnx` (was ~6 KiB placeholder).
  Acceptable: well under the 50 MiB ONNX size cap and roughly 3x
  `lpips_sq.onnx` (3.27 MiB), which is already shipped.
- **Neutral / follow-ups**:
  - **T6-7c**: train a luma-native FastDVDnet variant (or a
    chroma-aware luma+chroma extension) once the FFmpeg
    `vmaf_pre_temporal` filter (T6-7b proper, separate row) is in
    flight. The retrain target depends on the actual consumer surface.
  - **T6-7d** (optional): PTQ the wrapper graph to int8 once
    representative video-denoising calibration data exists; budget
    against an L1-residual gate on a held-out clip set.

## References

- Tassano, Delon, Veit. *FastDVDnet: Towards Real-Time Deep Video
  Denoising Without Flow Estimation*, CVPR 2020. arXiv:1907.01361.
- Upstream repo: `https://github.com/m-tassano/fastdvdnet` pinned at
  `c8fdf6182a0340e89dd18f5df25b47337cbede6f`.
- [ADR-0215](0215-fastdvdnet-pre-filter.md) — original
  contract + placeholder rationale.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI doc bar.
- [ADR-0174](0174-per-model-ptq.md) — per-model PTQ pattern (informs
  T6-7d).
- Source: per user direction (research+code task, "pick the smoke-only
  ONNX with the most accessible upstream weights and ship the real
  drop").
