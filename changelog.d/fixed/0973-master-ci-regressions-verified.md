# fix(ci): master CI regressions — Metal MS-SSIM fixture + ssimulacra2 icpx XYB (ADR-0973)

Fix two master CI regressions on master tip `4948b771c`, both verified
locally in the `vmaf-dev-mcp` container before being patched.

1. **macOS Metal MS-SSIM parity (3 jobs)**:
   `test_metal_float_ms_ssim_parity` was failing with
   `CPU: vmaf_read_pictures failed` because its `FIXTURE_H = 144u` sat
   below the `float_ms_ssim` minimum admissible dimension
   (`min_dim = GAUSSIAN_LEN << (SCALES - 1) = 11 << 4 = 176`, see
   `core/src/feature/float_ms_ssim.c:131-138`). The CPU twin's `init()`
   returned `-EINVAL` long before the Metal path ran. Bumped to
   `FIXTURE_H = 192u` (176 rounded up to a multiple of 16 for clean
   pyramid downsamples).

2. **Linux all-backends `test_ssimulacra2_simd::test_xyb`**: icx 2025.3 /
   2026.0 emits `vfmadd231ps` for the inline scalar reference
   `ref_linear_rgb_to_xyb` despite both `-ffp-contract=off` and
   `-fp-model=precise` being on the command line. The AVX2 SIMD lib uses
   explicit `_mm256_mul_ps` + `_mm256_add_ps` (no FMA intrinsics), so
   scalar diverged from SIMD by ~1 ULP per lane and the bit-exact
   `memcmp()` assertion failed. Added a file-scope
   `#pragma clang fp contract(off)` (with `-Wunknown-pragmas` suppression
   for GCC) to `core/test/test_ssimulacra2_simd.c` — empirically the only
   mechanism that suppresses icx's contraction. Production SIMD and
   production scalar paths are unchanged; no score drift.

Reproduction commands, captured pre- and post-fix output, and
compiler-asm forensics live in
[`docs/research/0973-master-ci-regressions-verified-2026-05-31.md`](../../docs/research/0973-master-ci-regressions-verified-2026-05-31.md).
