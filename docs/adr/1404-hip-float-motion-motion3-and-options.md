<!-- markdownlint-disable MD013 MD060 -->
# ADR-1404: `float_motion_hip` emits `motion3` and implements every CPU `float_motion` option on the device

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: hip, gpu-parity, motion, feature-extractor, fork-local

## Context

The CPU `float_motion` extractor emits `VMAF_feature_motion3_score` for every frame and takes nine options. `float_motion_hip` emitted `motion` and `motion2` only and declared four of the options (`debug`, `motion_force_zero`, `motion_fps_weight`, `motion_max_val`). Scores stayed correct, because the [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) gate keeps a feature with an undeclared option on the CPU, but `--backend hip --feature float_motion` dropped `motion3` from the output without a warning, a model reading `motion3` or setting `motion_blend_factor`, `motion_blend_offset`, `motion_filter_size`, `motion_add_scale1` or `motion_add_uv` never ran on the device, and naming the twin with one of them failed with `unknown option` (`T-HIP-FLOAT-MOTION-MOTION3-OPTIONS-2026-09-30`, the HIP part of `T-GPU-FLOAT-MOTION3-MISSING-2026-09-30`).

`motion3` and the two blend options are host arithmetic on the SAD the twin already reduces; the CUDA twin got them that way in #1637. The other three options change what the device computes: the blur filter, a second SAD over both blurred frames scaled to half size, and the same chain on the U and V planes. The state row allows either kernel work for these or an explicit `-ENOTSUP`, the posture `motion_hip` takes for `motion_add_uv` ([ADR-0989](0989-sycl-motion-add-uv.md)).

## Decision

`float_motion_hip` takes the CPU option table unchanged (names, aliases, types, defaults, ranges, order) and implements all of it:

1. **`motion3` on the host**, as `float_motion.c` forms it: `motion_blend()` from the shared `motion_blend_tools.h` of the fps-weighted score, then the `motion_max_val` cap; index 0 from the first SAD alone, then the blended `motion2`, the tail from `flush()`, 0 for a one-frame run and under `motion_force_zero`.
2. **`motion_filter_size` as a kernel argument.** The blur selects `FILTER_5_s`, `FILTER_3_s` (3) or `FILTER_5_NO_OP_s` (1) as `motion_blur_plane()` does. The 3-tap and no-op filters run as five taps with zero outer weights, which adds exact zeros, so the tile, its halo and the launch geometry are the same for every filter. The minimum frame is the CPU's: 2x2 for the 3-tap filter, 3x3 otherwise.
3. **`motion_add_scale1` as a second kernel.** `float_motion_hip_scale1_sad` scales both blurred frames with the bilinear scaler of `motion.c` (`motion_scale_bilinear()`, contraction off) and reduces `|cur - prev|` per block; the host adds the scale-1 mean to the scale-0 mean.
4. **`motion_add_uv` as the same kernels per plane.** The state holds one staging buffer and one blur ping-pong per plane; a frame uploads the planes it needs in one call and launches the blur (and scale-1) kernel once per plane with the plane's geometry (ceiling chroma extents from `vmaf_chroma_extent()`); all partials come back in one read-back. 4:0:0 input is refused at init, as on the CPU.

The two 8-bit / 16-bit kernel bodies become one template, so the tile load, the blur and the block reduction exist once.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Host `motion3` and blend options; filter, scale 1 and chroma on the device (chosen) | Every CPU `float_motion` request runs on the device; one option table; still one upload call, one read-back and one wait per frame | A second kernel and per-plane state; chroma triples the uploads when asked for | — |
| Host `motion3` and blend options only; `-ENOTSUP` for `motion_add_scale1` / `motion_add_uv`, default-only `motion_filter_size` ([ADR-1316](1316-gpu-option-value-capability-fallback.md)) | Small change, the CUDA twin's current scope | Three options still pin the feature to the CPU; a direct request for the twin fails at init | The kernel work is small: 26 ms a frame at 4K with both options against 75 ms on 16 CPU threads |
| Compute scale 1 or chroma on the host from read-back blurred planes | No new kernel | A full-resolution float plane per frame over the bus, a host pass per frame | Violates device residency |
| Round each plane's mean to fp32 as `vmaf_image_sad_c()` does | Mirrors the CPU's types | The CPU's fp32 running sum cannot be reproduced in parallel, so the rounding would not make the twin exact; the double sums are closer to the true value | No gain in parity |
| Separate 3-tap kernel with a 1-pixel halo | Fewer loads for `motion_filter_size=3` | A second tile geometry and entry-point pair for a rarely used option | The zero-weight taps cost nothing measurable |

## Consequences

- **Positive**: on a gfx1036 (`ryzen-4090-arc`, ROCm 7.2.4), `--precision max`, against `--backend cpu`: `motion3` within 2.78e-6 on the Netflix 576x324 pair and 5.71e-6 on BBB 3840x2160, the same as `motion2`; `motion_blend_factor=0.5:motion_blend_offset=2` 1.39e-6; `motion_filter_size=3` 5.65e-6 and `=1` 4.72e-7; `motion_add_scale1` 3.17e-6; `motion_add_uv` 2.73e-6; all options together 9.84e-6 (Netflix) and 8.45e-6 (4K); `motion_force_zero` and a one-frame run identical. `motion` and `motion2` with the previous options are bit-identical to the build before the change on every fixture. `--backend hip --feature float_motion=motion_add_uv=true` now lists `float_motion_hip` in `feature_backends`; before it warned and ran the CPU extractor.
- **Negative**: `float_motion_hip` carries three plane states, of which the default configuration uses one. The 1080p checkerboard pairs stay at 1.35e-4 from the CPU on a score of 18.86, as before: the CPU adds 2 million fp32 terms in one running sum.
- **Neutral / follow-ups**: the SYCL and Metal parts of `T-GPU-FLOAT-MOTION3-MISSING-2026-09-30` stay open, and so do `motion_filter_size`, `motion_add_scale1` and `motion_add_uv` on the CUDA, SYCL and Metal `float_motion` twins. Guarded by `test_hip_twin_option_parity` (device: every option against the CPU, the 2x2 boundary, the 4:0:0 and below-minimum refusals; device-free: the option table), six planted regressions in `test_hip_kernel_source_contract.py`, and `test_vmaf_feature_backend_hip` (the CLI runs the twin for `motion_filter_size=3`).

## References

- req: RC3 HIP lane brief (2026-10-01): "T-HIP-FLOAT-MOTION-MOTION3-OPTIONS-2026-09-30: float_motion_hip emits no motion3 score and lacks five CPU float_motion options."
- [ADR-1382](1382-hip-twin-cpu-option-parity.md) — the first four options and `motion_clip()`.
- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md), [ADR-1359](1359-cli-feature-backend-twin.md) — why an undeclared option kept the feature on the CPU.
- [ADR-0219](0219-motion3-gpu-coverage.md), [ADR-0989](0989-sycl-motion-add-uv.md) — host-side `motion3` and `motion_add_uv` on the integer twins.
- [Research-1372](../research/1372-cuda-rc3-parity-port.md) — the CUDA twin's host-side `motion3`.
