<!-- markdownlint-disable MD060 -->
# ADR-0580: float_ansnr enable_chroma option

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: lusoris
- **Tags**: `feature-extractor`, `metrics`

## Context

The `float_ansnr` feature extractor computed ANSNR and ANPSNR exclusively for
the luma (Y) plane. Every other extractor that operates on chrominance —
`integer_psnr` (`psnr_y/cb/cr`), `float_ssim` (`float_ssim_cb/cr`) — exposes
an `enable_chroma` flag so callers can opt in to per-plane scores. `float_ansnr`
was the only primary extractor lacking this surface, creating an asymmetry for
workflows that need chroma ANSNR scores without spinning up a full VMAF model.

## Decision

Add a `bool enable_chroma` option to `AnsnrState` (default `false`), mirroring
the pattern in `integer_psnr.c`. When enabled, the extractor loops over planes
0–2 and emits `float_ansnr`, `float_ansnr_cb`, `float_ansnr_cr`,
`float_anpsnr`, `float_anpsnr_cb`, `float_anpsnr_cr`. YUV400P input clamps the
plane count to 1 regardless of the flag.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Always compute chroma (remove luma-only mode) | Simpler API | Breaks existing callers that only want `float_ansnr`; increases compute cost for VMAF model pipelines that don't need it | Default `false` preserves back-compat |
| Separate `float_ansnr_chroma` extractor | Clean separation | Duplicates all state + init logic; two registration entries for one algorithm | Unnecessary complexity; `integer_psnr` precedent is a single extractor with a flag |

## Consequences

- **Positive**: callers can retrieve per-plane ANSNR/ANPSNR without a custom
  pipeline; consistent API surface with `integer_psnr`.
- **Negative**: GPU twins (CUDA, SYCL, Vulkan) do not yet expose this option;
  chroma ANSNR is CPU-only until a follow-up port.
- **Neutral / follow-ups**: GPU ports should emit the same four chroma feature
  names to avoid consumer-side format divergence.

## References

- `integer_psnr.c` `enable_chroma` implementation (same pattern).
- req: "Add `enable_chroma` to `float_ansnr` (mirror psnr/ssim pattern)."
