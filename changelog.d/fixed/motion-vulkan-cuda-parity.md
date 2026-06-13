### motion_vulkan: parity fixes vs CUDA/Metal/HIP (Metal #1018 + HIP #1037 audit)

Two host-side correctness fixes in `core/src/feature/vulkan/motion_vulkan.c`:

- **`extract_force_zero`**: removed the erroneous `frame_index > 0` guard that
  suppressed `motion_score` on frame 0. Every frame now receives `motion2_score=0`
  to anchor the collector index. `motion_score` and `motion3_score` are emitted
  only when `debug=true`, matching the CUDA/Metal/HIP pattern.

- **`flush`**: added `vmaf_feature_collector_get_score` idempotency probes for
  both `motion2_score` and `motion3_score` before the trailing tail append.
  Without the probes, a pending-collect that already wrote those scores at
  `frame_index-1` would cause a "cannot be overwritten" warning and surface as a
  context synchronisation error.
