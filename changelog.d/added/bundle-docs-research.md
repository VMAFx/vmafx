## Pure-docs research bundle: PR #111 + PR #115

Bundles two pure-documentation PRs into a single PR for merge-train efficiency.

**PR #111 — SIMD twin coverage inventory (ADR-0771, 2026-05-29)**

Added `docs/adr/0771-simd-twin-inventory.md` and
`docs/research/simd-twin-inventory-2026-05-29.md` — the first complete audit
of which feature extractors have AVX2, AVX-512, and NEON twins. Three true
gaps identified and prioritised for follow-up `/add-simd-path` work:

1. `integer_ssim` — zero SIMD coverage (highest leverage)
2. `integer_motion` NEON incomplete (missing `y_convolution` + `sad`)
3. `moment` / `float_moment` AVX-512 absent

**PR #115 — Thread-safety audit: CUDA / SYCL / HIP backends (ADR-0777, 2026-05-29)**

Added `docs/adr/0777-thread-safety-audit-gpu-backends.md` and
`docs/research/thread-safety-audit-backends-2026-05-29.md` — documents that
`VmafContext` handles are single-thread-only, SYCL frame-counters are
unprotected, CUDA uses the primary-context push/pop model, `log.c` statics
are not `_Atomic`, and no public ABI thread-safety contract exists. No fixes
applied; four follow-up items enumerated.
