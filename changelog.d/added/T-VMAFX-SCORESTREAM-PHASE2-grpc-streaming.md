- **gRPC `ScoreStream` is now real (ADR-0933 Phase 2).** The bidirectional
  `VmafxScoring.ScoreStream` RPC, previously a stub returning
  `codes.Unimplemented`, now performs per-frame VMAF scoring of in-memory raw
  frames via a new in-process `pkg/libvmaf.StreamScorer` (mirrors libvmaf's
  `vmaf_picture_alloc` + `vmaf_read_pictures` + `vmaf_score_at_index`). Clients
  push a `StreamConfig` then `FramePair` messages and receive one `FrameScore`
  per frame plus a terminal `AggregateScore`. Served by both `vmafx-server` and
  `vmafx-node`. The streaming pooled VMAF is bit-identical to the file-based
  `ScoreDirect` path.
- **`vmafx-node` now serves the `VmafxScoring` gRPC service (ADR-1109).** The
  node's `Serve()` — previously a listen-only stub registering no services —
  now exposes `Score`, `ScoreStream`, and `Health` on `VMAFX_NODE_ADDR`, with
  graceful shutdown on SIGTERM, turning each node into a directly-dispatchable
  scoring endpoint.
