<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1334: Extend MS-SSIM option parity to the Metal twin

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris
- **Tags**: `metal`, `ms-ssim`, `correctness`, `option-parity`, `testing`

## Context

ADR-1221 corrected `enable_db` and `clip_db` on the CUDA, SYCL, and HIP
MS-SSIM twins and explicitly left Metal as a separate gap. Metal still exposed
only `enable_lcs`, so valid CPU options were rejected at parse time, and it
could neither compute chroma-plane scores nor apply the geometry-derived dB
ceiling. The development fleet has no Apple GPU, so an Apple-only parity test
cannot be the sole regression gate.

The first parity-test implementation also reused one
`VmafFeatureDictionary` for CPU and Metal. `vmaf_use_feature()` consumes that
dictionary on every path after argument validation, making the second call a
use-after-free and the final explicit free a double-free. In production, a
non-finite per-scale L/C/S atom could also be erased by `pow(NaN, 0)` before
only the aggregate score was checked.

## Decision

Metal will expose `enable_lcs`, `enable_db`, `clip_db`, and `enable_chroma`,
compute active planes and geometry through a framework-free internal helper,
and derive the dB ceiling with ADR-1221's exact formula. The same helper is
compiled into an always-on C regression test on Linux, macOS, and Windows;
source-level mutation controls bind the Objective-C++ host to those tested
seams. The Apple parity test remains the device gate and constructs a fresh
option dictionary inside each CPU or Metal runner. Every plane's L/C/S atoms
are validated before the weighted product and before any collector append.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Framework-free semantic helper plus Apple device parity (chosen) | Executes the exact ceiling and plane-geometry logic on every host; retains end-to-end device evidence | Adds a small internal header and source-binding contract | — |
| Apple-device parity only | Exercises the complete kernel path | No Apple GPU in the development fleet; regressions stay green on Linux and Windows | Insufficient pre-merge signal |
| Source-string assertions only | Runs without Metal SDK | Can prove spelling but not option semantics; the prior max-dB test recomputed an unrelated value | Too shallow |
| Keep Metal luma-only and reject the options | Smallest implementation | Preserves the cross-backend user-visible gap | Does not close the pre-RC1 correctness item |

## Consequences

- **Positive**: Metal matches the CPU option meanings, advertises Cb/Cr scores,
  and fails a frame before publishing any result when a reduction atom is
  non-finite.
- **Positive**: device-free executable tests distinguish the correct 105 dB
  ceiling at 512x384 from both the former unbounded path and a geometry-free
  formula, and distinguish ceil-subsampled chroma geometry from truncation.
- **Negative**: the Metal host carries per-plane pyramid and partial buffers
  when chroma is enabled, increasing memory and dispatch work for that opt-in
  mode.
- **Neutral / follow-ups**: Metal hardware verification remains required on an
  Apple-Silicon runner before claiming measured device parity. ADR-1221 remains
  unchanged as the historical decision for CUDA, SYCL, and HIP.

## References

- [ADR-1221](1221-gpu-ms-ssim-db-ceiling.md) — dB-ceiling semantics for the
  CUDA, SYCL, and HIP twins; explicitly identifies Metal as separate scope.
- [ADR-1302](1302-nonfinite-scores-fail-the-frame.md) — validate enabled metric
  outputs before clamping or publication.
- [Research-2110](../research/2110-metal-ms-ssim-option-parity-2026-09-25.md)
  — implementation, ownership failure, and red-cap evidence.
- `T-GAP-METAL-MS-SSIM-DB-CHROMA-OPTIONS-2026-09-07` in `docs/state.md`.
- Source: `req` — “we fix everything until we cant find anything anymore for now”.
