<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1206: Every CUDA parity test also runs against a second, larger fixture

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: testing, cuda, ci, correctness

## Context

Of the 77 cross-backend parity tests in `core/test/`, 67 pinned a single
`256x144` fixture and the rest pinned one other small size. That is a
systematic blind spot, not an incidental one: several extractors change
behaviour with resolution, and none of those branches were reachable at the
pinned size.

Concretely, the shared SSIM / MS-SSIM auto-scale is
`max(1, round(min(w, h) / 256))`, so it is always `1` below `min(w, h) = 384`;
and the ADM border crop is `(int)(dim * 0.1 - 0.5)`, which is `0` only for
small bands. Two real divergences hid behind exactly this gap — the
speed_chroma 4K defect ([ADR-1202](1202-cuda-speed-chroma-4k-launch-bounds.md))
and the float-ADM edge-indexing defect
([ADR-1204](1204-adm-cm-edge-clamp-gpu-twins.md)). Having the same gap produce
two independent bugs makes closing it more valuable than either individual fix.

Adding a second fixture immediately paid for itself: it is what surfaced the
`float_ssim_cuda` scale=1-only refusal at real resolutions, and it re-confirmed
the ADM defect from the width side as well as the height side.

## Decision

We will register a `_large` variant of every CUDA parity test, built from the
same translation unit against a `960x540` fixture. `960x540` is chosen because
`min(w, h) = 540` puts the auto-scale at 2 (crossing the decimation boundary)
and `540` is not a multiple of the 16/32-wide kernel blocks, so tail-bound
handling is exercised as well. The fixture macros become
`#ifndef`-guarded so the second size is supplied by `-D` from `meson.build`
with no duplicated test source.

Where a GPU twin legitimately refuses the larger resolution — `float_ssim_cuda`
is a documented v1 scale=1-only extractor — the variant asserts that
*documented contract* (clean refusal, reported as a skip) rather than being
dropped. That way, if the twin ever stops refusing and starts returning a
scale=1 score at a decimating resolution, the test fails instead of silently
comparing two different metrics.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Second fixture size per test via `-D` and a meson `foreach` (chosen) | No duplicated test source; one obvious knob; catches the whole class | 16 extra small test binaries to build and run | — |
| Change the existing fixture to 960x540 instead of adding a variant | No new targets | Loses coverage of the small-band paths, which is where the ADM defect actually lives | Rejected — would trade one blind spot for another |
| Randomise the fixture size per run | Broadest coverage over time | Non-deterministic CI; a failure may not reproduce | Rejected — parity gates must be reproducible |
| Add sizes only to the tests known to be resolution-sensitive | Cheapest | Requires knowing which those are, which is the thing we got wrong twice | Rejected |

## Consequences

- **Positive**: resolution-dependent divergence is now a tested property rather
  than something found by accident. The 16 variants run in ~2-6 s each.
- **Negative**: 16 additional test binaries to compile and run in the GPU lane.
  Build and runtime cost is small but not zero.
- **Neutral / follow-ups**: only the CUDA family is covered here, because this
  workstation has a free CUDA device to verify against. Extending the same
  pattern to the SYCL / HIP / Metal parity families is a follow-up.

## References

- [ADR-0214](0214-gpu-parity-ci-gate.md) — the places=4
  cross-backend tolerance these tests gate on.
- Bugs found by this change: [ADR-1204](1204-adm-cm-edge-clamp-gpu-twins.md),
  [ADR-1205](1205-ssimulacra2-fma-unification-scalar-and-gpu.md).
- Source: `req` — user direction to fix the outstanding GPU parity failures.
