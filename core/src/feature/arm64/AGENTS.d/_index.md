<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src/feature/arm64

Agent orientation: NEON / SVE2 feature SIMD paths. Parent rules apply in full: [../AGENTS.md](../../AGENTS.md), [../../AGENTS.md](../../../AGENTS.md). Sister directory: [`../x86/`](../../x86/AGENTS.md).

- **Scalar bit-exactness non-negotiable**: every NEON / SVE2 kernel mirrors scalar TU byte-for-byte under `FLT_EVAL_METHOD == 0` (same contract as AVX2 / AVX-512); bit-exact tests in [`../../../test/`](../../../../test/) (`test_*_simd.c`, [`simd_bitexact_test.h`](../../../../test/simd_bitexact_test.h) harness, ADR-0245) catch ULP drift.
- **MSVC compiles this directory** (ADR-1260, `Windows ARM64 MSVC` CI job): `<arm_neon.h>` on every compiler; no GCC vector extensions on NEON types (`v[0]`, `a + b`, brace/compound-literal vectors), no `_x2`/`_x3`/`_x4` multi-register loads, and `__attribute__` / `#pragma GCC` / `#pragma clang` only under `#if defined(__GNUC__)` or `defined(__clang__)`. Strict FP comes from `vmaf_strict_fp_args` in `core/src/meson.build` (`/fp:precise` on MSVC, `/fp:precise /clang:-fno-fast-math /clang:-fcomplex-arithmetic=full /clang:-ffp-contract=off` on `intel-llvm-cl`, `/clang:-ffp-contract=off` on `clang-cl`, `-ffp-contract=off` on GCC/Clang); never restore literal strict-FP flags in arm64 `c_args`. SVE2 TUs skip MSVC (no `<arm_sve.h>`); QEMU cross-build covers GCC/Clang only.
