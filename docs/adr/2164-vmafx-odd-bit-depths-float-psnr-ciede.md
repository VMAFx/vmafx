<!-- markdownlint-disable MD013 MD060 -->
# ADR-2164: float_psnr and ciede take every depth the engine reads (8 to 16 bits); psnr_hvs stays at 12

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: maintainer (orchestrator decision of 2026-10-07); RC4 WP13 follow-up
- **Tags**: api, rc4, correctness, gpu, cli

## Context

The engine reads 8 to 16 bits per component. [ADR-2145](2145-vmafx-input-format-table.md)
made 9 and 14 bits (and 11, 13, 15) reachable from the command line and the
import, which exposed three limits: `float_psnr` allocated its peak and ceiling for
8, 10, 12 and 16 bits only; `ciede` refused every depth but those four at init;
and `picture_copy()`, the float normaliser behind every `float_*` extractor,
divided by 4, 16 or 256 for 10, 12 and 16 bits and read any other high depth as
8-bit bytes. The last one did not refuse: `float_ssim`, `float_ms_ssim`,
`float_adm`, `float_vif` and `float_motion` returned numbers for 9-, 11-, 13-,
14- and 15-bit input that were computed from the low and high bytes of each word.

## Decision

1. `picture_copy()` divides by `2^(bpc - 8)` for every depth above 8 (4, 16 and
   256 as before at 10, 12 and 16 bits).
2. `float_psnr` uses the peak `(2^bpc - 1) / 2^(bpc - 8)` and the ceiling
   `6 * bpc + 12` at every depth. At 8, 10, 12 and 16 bits these are the
   literals it had (255, 255.75, 255.9375, 255.99609375; 60, 72, 84, 108): the
   doubles are equal. The CUDA, HIP and SYCL twins take the same expression; the
   Metal twin refuses the new depths by name (`-EINVAL`) until it has run on an
   Apple device (`requests/WP13-5.md`).
3. `ciede` accepts 9 to 16 bits (its conversion to Lab is generic in the
   depth). The HIP twin's constant table has one entry per depth (`bpc - 8`); the
   CUDA and SYCL twins compute the constants from the depth and needed no change.
4. `psnr_hvs` above 12 bits stays refused: the limit is the algorithm's (the Xiph
   source); it is a row of the format envelope tables (ADR-1880, RC6 / RC7).
5. The twins stay bit-identical to the CPU (`float_psnr`) or within `1e-9` (`ciede`,
   libm) at the new depths: `test_{cuda,hip,sycl}_float_psnr_parity` and
   `test_{cuda,hip,sycl}_ciede_parity` carry the depths.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Generalise the formulas (chosen) | One expression, no new tables | Twins change in the same PR | Chosen |
| Refuse odd depths by name in the CLI | No extractor change | A defect: the engine reads them and `psnr` already scores them | Rejected (maintainer) |
| Per-depth literals for 9, 11, 13, 14, 15 | Mirrors the old code | Five more literals per twin to keep in step | Rejected |

## Consequences

- **Positive**: every extractor but `psnr_hvs` (and the Metal `float_psnr`) scores
  every depth the CLI and the import deliver; the silent wrong scores of the
  `float_*` extractors at those depths are gone.
- **Negative**: the other `float_*` GPU twins at odd depths are not measured here
  (state row `T-FLOAT-EXTRACTORS-ODD-DEPTH-TWINS-UNMEASURED-2026-10-07`).
- **Neutral / follow-ups**: Metal `float_psnr` (WP13-5).

## References

- [ADR-2145](2145-vmafx-input-format-table.md), [ADR-2146](2146-vmafx-rgb-input-explicit-matrix.md).
- Orchestrator decision (2026-10-07): float_psnr and ciede at 9, 11, 13, 14 and 15 bits with their exact twins in the same PR, bit-exact; psnr_hvs above 12 bits stays refused and documented.
- Tests: `core/test/test_odd_depths_float_psnr_ciede.c` (CPU, independent oracle), the `*_odd_depths*` cases of the twin parity tests.
