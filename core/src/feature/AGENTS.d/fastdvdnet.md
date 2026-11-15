---
paths:
  - core/src/feature/fastdvdnet_pre.c
invariant: FastDVDnet 5-frame-window buffering and temporal pre-filter lifecycle contracts.
area: ssim-and-metrics
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# FastDVDnet 5-Frame Window Contract and Prefilter

- **`fastdvdnet_pre.c` 5-frame-window contract** (fork-local,
  ADR-0215): FastDVDnet temporal pre-filter extractor is wired
  to I/O contract `frames: float32 NCHW [1, 5, H, W]` (channel
  axis stacks `[t-2, t-1, t, t+1, t+2]`) → `denoised: float32 NCHW
  [1, 1, H, W]`. Three pieces are load-bearing on rebase: (1)
  centre index is 2 (`FASTDVDNET_PRE_CENTRE`) — `gather_window`
  computes channel-k offsets relative to it; (2) ring buffer
  holds 5 slots and replicates closest available end frame for
  channel positions outside available window (clip start +
  end); (3) registered feature name is
  `fastdvdnet_pre_l1_residual` — downstream consumers (future
  FFmpeg `vmaf_pre_temporal` filter, training harnesses) bind to
  that exact string. **T6-7b update (ADR-0255)**: registry now
  ships real upstream FastDVDnet weights (`smoke: false`) wrapped by
  luma adapter in `ai/scripts/export_fastdvdnet_pre.py`;
  previous smoke-only placeholder is history. C-side contract is
  unchanged; wrapper keeps I/O names (`frames` / `denoised`)
  byte-identical, handles `Y → [Y, Y, Y]` tiling, supplies
  constant `sigma = 25/255` noise map, and performs BT.601 RGB→Y
  collapse internally. Two rebase-sensitive invariants flow from
  wrapper: (4) upstream's `nn.PixelShuffle` is swapped for
  allowlist-safe `Reshape`/`Transpose`/`Reshape` decomposition at
  export time (`DepthToSpace` is not on ONNX op allowlist —
  ADR-0255 §Decision); (5) upstream commit is pinned at
  `c8fdf6182a0340e89dd18f5df25b47337cbede6f` and exporter
  enforces upstream weights sha256
  `9d9d8413c33e3d9d961d07c530237befa1197610b9d60602ff42fd77975d2a17`
  to keep weights drop reproducible. See
  [ADR-0215](../../../../docs/adr/0215-fastdvdnet-pre-filter.md) and
  [ADR-0255](../../../../docs/adr/0255-fastdvdnet-pre-real-weights.md).

- **FastDVDnet temporal pre-filter (T6-7, PR #203 MERGED, ADR-0215)**
  — 5-frame window pre-filter feeding ssim/ms_ssim. DNN-backed.
