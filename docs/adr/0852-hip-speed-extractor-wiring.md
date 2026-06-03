# ADR-0852: Wire speed_chroma_hip and speed_temporal_hip into HIP Build and Dispatch

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `hip`, `build`, `speed`, `feature`, `gpu`, `fork-local`

## Context

ADR-0567 landed real on-device GPU kernels for `speed_chroma` and `speed_temporal`
on all four backends (CUDA, SYCL, HIP, Vulkan). The HIP half produced three files:

- `core/src/feature/hip/speed/speed_score.hip` — five `extern "C"` kernels
- `core/src/feature/hip/speed_chroma_hip.c` — host wrapper, `vmaf_fex_speed_chroma_hip`
- `core/src/feature/hip/speed_temporal_hip.c` — host wrapper, `vmaf_fex_speed_temporal_hip`

All three files were committed but none were wired into the build system or the
feature extractor registry. The `hip_kernel_sources` meson dict in
`core/src/meson.build` had no `speed_score` entry, `core/src/hip/meson.build` had
no entries for the two `.c` wrappers, and `core/src/feature/feature_extractor.c`
had no `extern` declarations or dispatch-table entries for the two extractors.
This left the SpEED HIP path unreachable regardless of build flags.

## Decision

We add the three missing wiring entries (one in each of the three files listed
above). No implementation changes are made — only build-system and registry
connections. The scaffold posture is preserved: without `enable_hipcc=true` both
extractors' `init()` returns `-ENOSYS`, matching every prior HIP consumer's contract.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Wire all three simultaneously (chosen) | Closes the gap atomically; consistent with ADR-0533 pattern | Slightly larger changeset than wiring only one side | Wiring only host wrappers without the kernel blob gives a guaranteed -ENOSYS on `hipModuleLoadData` — no user benefit |
| Add a weak HSACO stub and defer kernel wiring | Keeps link green, mirrors the pre-ADR-0539 ADM pattern | Users get -ENOSYS on the kernel load, same as without the stub | The real kernel source already exists; there is no reason to defer it |

## Consequences

- **Positive**: `speed_chroma_hip` and `speed_temporal_hip` are now reachable via
  `vmaf_get_feature_extractor_by_name`; with `enable_hipcc=true` they run real
  on-device kernels per ADR-0567.
- **Negative**: none — the implementation was already reviewed as part of ADR-0567.
- **Neutral / follow-ups**: ADR-0214 cross-backend ULP gate (`places=4` vs CPU
  reference) should be run once a ROCm device is available in CI.

## References

- ADR-0567 (`0567-speed-chroma-temporal-real-gpu.md`) — original implementation.
- ADR-0533 (`0533-hip-extractor-registration-sweep.md`) — HIP extractor wiring pattern.
- ADR-0539 (`0539-integer-adm-hip-kernels.md`) — prior art for `hip_kernel_sources`.
- `core/src/feature/hip/AGENTS.md` — `hip_hsaco_stubs.c` fallback pattern.
