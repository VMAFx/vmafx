<!-- markdownlint-disable MD013 MD060 -->
# ADR-0266: HIP fifth kernel-template consumer — `float_ansnr_hip`

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris, Claude (T7-10b follow-up)
- **Tags**: `hip`, `gpu`, `feature-extractor`, `kernel-template`

## Context

The HIP backend's kernel-template scaffold ([ADR-0241](0241-hip-first-consumer-psnr.md)) needs a stable
quorum of consumers before the runtime PR (T7-10b) can flip the
`kernel_template.c` helper bodies from `-ENOSYS` to live HIP calls.
Each consumer pins a *shape* of usage that the runtime PR's helper
bodies must satisfy: 1-counter atomic readback (`integer_psnr_hip`,
ADR-0241), float-partial readback (`float_psnr_hip`, ADR-0254),
single-float-per-block bypass (`ciede_hip`, ADR-0259), and four-counter
atomic readback (`float_moment_hip`, ADR-0260) are the four already
landed.

This ADR adds a **fifth consumer** — `float_ansnr_hip`, the HIP twin
of `core/src/feature/cuda/float_ansnr_cuda.c` (297 LOC). It pins a
new shape: **interleaved (sig, noise) per-block float partials** where
two doubles are reduced on the host before the score formula. Same
`submit_pre_launch` bypass as `ciede_hip` (no atomic, no memset
required), but doubles the partial-element width and exercises the
post-reduction two-feature emission path (`float_ansnr` +
`float_anpsnr` from a single kernel pass).

## Decision

We add `core/src/feature/hip/float_ansnr_hip.{c,h}` as the fifth
kernel-template consumer. The TU mirrors the CUDA twin
call-graph-for-call-graph: identical state struct, identical
`init/submit/collect/close` lifecycle, identical template-helper
invocations. `init()` returns `-ENOSYS` until T7-10b lands the runtime
helpers; the smoke test pins the registration shape; CHANGELOG and
rebase-notes carry the addition.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `float_ansnr_hip` (chosen) | Smallest unported CUDA twin (297 LOC); pins the (sig, noise) interleaved-partials readback shape that no prior consumer exercises; same `submit_pre_launch` bypass as `ciede_hip` so the runtime PR's helper-body work is the same shape we already validated. | Doubles per-block partial width vs the third consumer. | — |
| `integer_motion_v2_hip` | Pins the temporal-extractor + ping-pong-buffer shape. | Different concern — taken as the sixth consumer in the same PR ([ADR-0267](0267-hip-sixth-consumer-motion-v2.md)). | — |
| `float_motion_hip` (361 LOC) | Single-dispatch motion mirror. | 21% larger than `integer_motion_v2_cuda.c`; overlaps the temporal-extractor shape that `motion_v2_hip` already pins. | Defer to a later batch. |
| `integer_ssim_hip` (384 LOC) | Multi-feature emission. | 30% larger than `float_ansnr_cuda.c`. | Defer to a later batch. |

## Consequences

- **Positive**: One more concrete shape (`(sig, noise)` interleaved
  float partials) is now pinned ahead of T7-10b, reducing the surface
  area the runtime PR has to validate against. The runtime PR's
  cross-backend-diff gate ([ADR-0214](0214-gpu-parity-ci-gate.md))
  inherits a fifth consumer with no kernel work: just the standard
  helper-body flip.
- **Negative**: One more `#if HAVE_HIP` extractor row in
  `feature_extractor.c` to keep aligned with the runtime PR's
  picture buffer-type plumbing.
- **Neutral / follow-ups**: AGENTS.md note pins the
  `submit_pre_launch` bypass as a load-bearing invariant. The
  runtime PR will swap the `-ENOSYS` body for live calls without
  touching this TU.

## References

- [ADR-0212](0212-hip-backend-scaffold.md) — HIP audit-first scaffold (T7-10).
- [ADR-0241](0241-hip-first-consumer-psnr.md) — first consumer + kernel template.
- [ADR-0254](0254-hip-second-consumer-float-psnr.md) — second consumer (`float_psnr_hip`, PR #324).
- [ADR-0259](0259-hip-third-consumer-ciede.md) — third consumer (`ciede_hip`, PR #330).
- [ADR-0260](0260-hip-fourth-consumer-float-moment.md) — fourth consumer (`float_moment_hip`, PR #330).
- [ADR-0267](0267-hip-sixth-consumer-motion-v2.md) — sixth consumer (this PR).
- [ADR-0246](0246-gpu-kernel-template.md) — GPU kernel-template pattern.
- CUDA twin: `core/src/feature/cuda/float_ansnr_cuda.c`.
- Source: `req` (user dispatch) — "Add the fifth and sixth HIP runtime kernel-template consumers. ... Pick two more cleanest CUDA twins to port."
