<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-0989: Wire motion_add_uv through integer_motion_sycl; emit warning on motion_five_frame_window

- **Status**: Accepted
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: `sycl`, `motion`, `feature-extractor`, `gpu`

## Context

The `float_motion` CPU extractor exposes a `motion_add_uv` option (alias `mau`)
that adds the blurred U- and V-plane SADs (normalized per-plane) to the Y-plane
SAD, producing a combined Y+U+V motion score. This option was silently ignored
by every GPU backend: the `integer_motion_sycl` extractor accepted the option
but never applied it, so callers that set `motion_add_uv=true` received the
Y-only score without any indication of the discrepancy.

Additionally, the `motion_five_frame_window` option (already rejected with
`-ENOTSUP` at init time) logged at `VMAF_LOG_LEVEL_ERROR` in SYCL and
Vulkan, while returning without a warning message at all in CUDA and HIP.
The per-backend option-table sync invariant (AGENTS.md) requires all five GPU
backends (SYCL, CUDA, Vulkan, HIP, Metal) to expose the same option surface.

## Decision

1. Wire `motion_add_uv` through `integer_motion_sycl.cpp` as the lead
   implementation. When enabled, the SYCL extractor allocates additional
   ping-pong device buffers for raw U/V pixels and blurred U/V pixels, plus
   separate SAD accumulators. UV planes are uploaded H2D in `submit_fex_sycl`
   before the graph fires. Additional `launch_blur_sad_fused` kernels for U
   and V are launched in `enqueue_motion_work` (graph-recorded). Y + U + V
   SAD contributions are summed on the host in `collect_fex_sycl`, each
   normalized by its plane area.

2. Add `motion_add_uv` as a recognized-but-rejected option (returns
   `-ENOTSUP` with a `VMAF_LOG_LEVEL_WARNING` message) to the four
   remaining GPU backends: CUDA, Vulkan, HIP, Metal. This keeps the
   option table in sync and directs users to the SYCL backend.

3. Upgrade `motion_five_frame_window` rejection messages to
   `VMAF_LOG_LEVEL_WARNING` across all backends for consistency.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Implement UV on all 5 backends in one PR | Complete parity | Vulkan shader + CUDA + HIP + Metal kernel changes in a single DRAFT PR is too wide | Deferred follow-up acceptable for DRAFT |
| Opt-out UV from graph (direct-enqueue path) | Simpler | Breaks combined-graph optimization | Graph approach is consistent with existing motion architecture |
| Return -EINVAL (not -ENOTSUP) for unsupported options | Slightly more correct POSIX semantics for "not implemented" | Inconsistent with existing `motion_five_frame_window` posture | Keep convention consistent |

## Consequences

- **Positive**: `motion_sycl` with `motion_add_uv=true` faithfully computes
  the same Y+U+V motion score as CPU `float_motion(motion_add_uv=true)`
  within ADR-0214 places=4 tolerance. CHUG / K150K sweeps that set `mau=true`
  get the expected score rather than a silent Y-only fallback.
- **Positive**: All five GPU backends now surface `motion_add_uv` in their
  option table; passing the option either works (SYCL) or fails loudly with
  a warning (-ENOTSUP on CUDA/Vulkan/HIP/Metal).
- **Positive**: `motion_five_frame_window` rejection messages are consistently
  `WARNING` across all backends.
- **Negative**: CUDA, Vulkan, HIP, and Metal still do not implement UV blending;
  follow-up kernel work is tracked by the ADR-0989 deferred label in each
  backend's help string.
- **Neutral / follow-ups**: Metal gains its first real option entry (was `{0}`).
  Future ports of UV support to other backends should cite this ADR.

## References

- [ADR-0219](0219-motion3-gpu-contract.md) — motion3 GPU contract (motion_five_frame_window ENOTSUP rationale).
- [ADR-0214](0214-gpu-parity-ci-gate.md) — cross-backend parity gate places=4.
- `req`: the user directed wiring `motion_add_uv` through `integer_motion_sycl.cpp`
  parallel to the CPU implementation, and adding a clear runtime warning for
  `motion_five_frame_window`.
