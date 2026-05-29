## SIMD twin coverage inventory (ADR-0771)

Added `docs/adr/0771-simd-twin-inventory.md` and
`docs/research/simd-twin-inventory-2026-05-29.md` — the first complete audit
of which feature extractors have AVX2, AVX-512, and NEON twins. Three true
gaps identified and prioritised for follow-up `/add-simd-path` work:

1. `integer_ssim` — zero SIMD coverage (highest leverage)
2. `integer_motion` NEON incomplete (missing `y_convolution` + `sad`)
3. `moment` / `float_moment` AVX-512 absent

PR: see `docs/simd-twin-inventory-0771` branch.
