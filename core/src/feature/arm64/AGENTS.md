<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src/feature/arm64

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../../docs/development/agents-index.md)).

Agent orientation: NEON / SVE2 feature SIMD paths. Parent rules apply in full: [../AGENTS.md](../AGENTS.md), [../../AGENTS.md](../../AGENTS.md). Sister directory: [`../x86/`](../x86/AGENTS.md).

- **Scalar bit-exactness non-negotiable**: every NEON / SVE2 kernel mirrors scalar TU byte-for-byte under `FLT_EVAL_METHOD == 0` (same contract as AVX2 / AVX-512); bit-exact tests in [`../../../test/`](../../../test/) (`test_*_simd.c`, [`simd_bitexact_test.h`](../../../test/simd_bitexact_test.h) harness, ADR-0245) catch ULP drift.
- **MSVC compiles this directory** (ADR-1260, `Windows ARM64 MSVC` CI job): `<arm_neon.h>` on every compiler; no GCC vector extensions on NEON types (`v[0]`, `a + b`, brace/compound-literal vectors), no `_x2`/`_x3`/`_x4` multi-register loads, and `__attribute__` / `#pragma GCC` / `#pragma clang` only under `#if defined(__GNUC__)` or `defined(__clang__)`. Strict FP comes from `vmaf_strict_fp_args` in `core/src/meson.build` (`/fp:precise` on MSVC, `/fp:precise /clang:-fno-fast-math /clang:-fcomplex-arithmetic=full /clang:-ffp-contract=off` on `intel-llvm-cl`, `/clang:-ffp-contract=off` on `clang-cl`, `-ffp-contract=off` on GCC/Clang); never restore literal strict-FP flags in arm64 `c_args`. SVE2 TUs skip MSVC (no `<arm_sve.h>`); QEMU cross-build covers GCC/Clang only.

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `cambi_neon.*`, `*sve2*`, `ssimulacra2*` | [cambi-sve2-and-ssimulacra2](AGENTS.d/cambi-sve2-and-ssimulacra2.md) | CAMBI NEON scan/tail rules, SVE2 4-lane versus VLA raster reductions, and shared SSIMULACRA 2 ARM64 header. |
| `speed_neon.c`, `psnr_hvs_neon.c`, `/core/src/meson.build` | [fp-targets-speed-and-msvc](AGENTS.d/fp-targets-speed-and-msvc.md) | arm64_v8_fp strict-FP library placement, pointer accumulators, and SpEED covariance. |
| `**`, `/.github/workflows/lint-and-format.yml` | [scaffolding-lint-and-adrs](AGENTS.d/scaffolding-lint-and-adrs.md) | New NEON/SVE2 TU scaffolding, arm64-only clang-tidy ratchet measurement, upstream sync, and governing ADRs. |
| `*adm*neon*`, `*.h` | [scope-and-adm-neon](AGENTS.d/scope-and-adm-neon.md) | Self-contained SIMD headers and integer/float ADM NEON non-contracting kernels. |
| `*.c`, `/core/src/feature/x86/*.c`, `/core/src/feature/iqa/convolve.c`, `/core/src/feature/iqa/ssim_tools.c`, `/core/src/feature/iqa/ssim_accumulate_lane.h`, `/core/src/feature/ms_ssim_decimate.c`, `/core/src/feature/third_party/xiph/psnr_hvs.c`, `/core/src/feature/ssimulacra2.c`, `/core/src/feature/cambi.c`, `/core/src/feature/cambi_c_values_frame.h`, `/core/src/feature/moment.c`, `/core/test/test_psnr_hvs_dispatch_invariance.c`, `/core/test/test_cambi_stage_simd.c`, `/core/test/test_cambi_spatial_mask_simd.c`, `/core/test/test_cambi_dispatch_invariance.c`, `/core/test/test_cambi_simd.c`, `/core/test/test_moment_simd.c` | [twin-update-rules](AGENTS.d/twin-update-rules.md) | Lockstep twin-bundle updates across ARM64 NEON/SVE2, x86 AVX2/AVX-512, and scalar references. |
