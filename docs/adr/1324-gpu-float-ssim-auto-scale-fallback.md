<!-- markdownlint-disable MD013 MD060 -->
# ADR-1324: Resolve GPU float-SSIM auto-scale before backend initialization

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris
- **Tags**: `gpu`, `feature-options`, `dispatch`, `correctness`, `testing`

## Context

The CPU `float_ssim` extractor resolves `scale=0` from the first picture as
`max(1, round(min(width, height) / 256))`. The CUDA, SYCL, HIP and Metal twins
mirror that option schema but currently implement only resolved scale `1`.
Consequently the declared default succeeds through a short side of 383 pixels,
then fails backend initialization with `-EINVAL` at 384 pixels and above.
Common 960x540 and 1920x1080 inputs therefore fail after automatic model
dispatch selects a GPU twin, even though the CPU extractor can honour the same
option dictionary.

[ADR-1316](1316-gpu-option-value-capability-fallback.md) deliberately cannot
represent this restriction: `scale=0` is a supported value, and its capability
depends on dimensions that are not necessarily known when a model is
registered. Marking it default-only or narrowing the option range would also
misrepresent the working small-frame and explicit `scale=1` paths.

## Decision

Add an internal, optional first-picture capability callback and a named CPU
fallback to `VmafFeatureExtractor`. Contexts registered from a model are
eligible for this fallback; contexts registered by explicitly naming an
extractor are not. After the first host-picture pair establishes format, bit
depth and dimensions and backend preparation completes, but before extractor
initialization or CUDA picture translation, libvmaf asks each eligible
extractor whether that context is supported. An `-ENOTSUP` result replaces
only that uninitialized context with the named CPU extractor, preserving a
private copy of the same parsed option dictionary and invalidating cached CUDA
residency requirements. The SYCL host path may already have uploaded the pair
into its shared staging buffers during preparation; no extractor has been
initialized or submitted at that point.

The four GPU `float_ssim` twins use their existing scale helpers for this
check and name `float_ssim` as the fallback. Any response other than
`-ENOTSUP` remains an error. Invalid or out-of-range option values still fail
during context creation, and direct `float_ssim_{cuda,sycl,hip,metal}` requests
still reach the backend's existing `scale=1` initialization guard.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Implement decimation in all four GPU kernels | Keeps every supported scale on device | Four independent kernel and allocation changes require hardware validation on NVIDIA, Intel, AMD and Apple before RC1 | Deferred as backend completion and performance work |
| Mark `scale` default-only under ADR-1316 | Reuses the value-aware selection gate | Incorrectly sends working `scale=0` small frames to CPU and cannot see dimensions | Rejected: option value alone is insufficient |
| Narrow the GPU option range to `scale=1` | Makes the restriction visible at parse time | Breaks CPU/GPU schema identity and rejects the valid automatic small-frame path | Rejected: schema and contextual capability are different facts |
| Retry CPU after GPU `init()` returns `-EINVAL` | Avoids a new pre-init callback | Conflates malformed input and device/runtime failures with capability, and runs after picture residency has already been chosen | Rejected: fallback must be explicit and pre-init |
| Add a dimension-aware pre-init fallback | Reuses CPU's complete implementation, changes no arithmetic, and is testable without four devices | A mixed model may compute this one feature on CPU until GPU decimation exists | **Chosen** |

## Consequences

- **Positive**: Automatic model dispatch completes on small, threshold and
  common broadcast dimensions without turning an unsupported GPU scale into a
  whole-run failure.
- **Positive**: CUDA, SYCL, HIP and Metal share one lifecycle rule, while each
  backend remains the authority for its own scale calculation.
- **Positive**: Parser failures, explicit extractor selection, score
  arithmetic, models, snapshots and Netflix golden assertions are unchanged.
- **Negative**: When auto-scale resolves above `1`, `float_ssim` alone runs on
  the CPU and incurs the corresponding host work.
- **Neutral / follow-ups**: The device-buffer-only
  `vmaf_read_pictures_sycl()` API has no host pictures on which a CPU extractor
  can run, so it retains its scale-1-only contract. A future verified SYCL
  decimation port can remove that limitation. Adding real decimation to any
  twin removes its callback only after dimension parity coverage passes on
  that backend.
- **Neutral**: This changes no public C header, CLI syntax, Meson option,
  FFmpeg patch surface, dependency or release artifact.

## References

- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) — model option-name capability gate.
- [ADR-1316](1316-gpu-option-value-capability-fallback.md) — value-aware capability gate and direct-selection boundary.
- [Research-2108](../research/2108-gpu-float-ssim-auto-scale-fallback-2026-09-25.md) — reproduction, inventory and verification.
- Source: `req` — “we fix everything until we cant find anything anymore for now”.
