# AGENTS.md — core/src/feature/x86

Orientation for agents working on the AVX2 / AVX-512 feature SIMD
paths. Parent: [../AGENTS.md](../AGENTS.md).

## Scope

Per-feature AVX2 + AVX-512 SIMD implementations. Every TU here mirrors
a scalar reference one level up (e.g. `ssim_avx2.c` ↔ `../iqa/ssim_tools.c`,
`adm_avx2.c` ↔ `../adm.c`) and is dispatched at runtime from a feature's
`*_dispatch.c` based on `vmaf_get_cpu_flags_x86()` (see
[`../../x86/cpu.c`](../../x86/cpu.c)).

```text
feature/x86/
  <feature>_avx2.{c,h}      # AVX2 path (Haswell+ baseline)
  <feature>_avx512.{c,h}    # AVX-512 path (Skylake-X+ baseline; ICL flag for AVX512BW/VBMI2)
  ms_ssim_decimate_*.{c,h}  # 9-tap LPF SIMD (one of four byte-identical TUs — see parent AGENTS.md)
```

The cross-feature plumbing (dispatch tables, the `simd_dx.h` macro
header, the runtime CPUID gate) lives in `../` — this directory
contains only kernel TUs.

## Ground rules

- **Every SIMD `.h` file MUST be self-contained.** Include every
  standard header that names a type used in the file's own declarations
  — do not rely on transitive includes from consumer `.c` files. In
  particular, any header that declares a `ptrdiff_t` parameter MUST
  include `<stddef.h>` directly. Standalone-include failures on Apple
  Clang and Ubuntu ARM Clang are CI regressions (see PR #914 for the
  cambi family; fixed for the motion family in the accompanying PR).
- **Parent rules** apply in full (see [../AGENTS.md](../AGENTS.md) +
  [../../AGENTS.md](../../AGENTS.md)).
- **Bit-exactness with the scalar reference is non-negotiable.** Every
  AVX2 / AVX-512 kernel here mirrors a scalar TU byte-for-byte under
  `FLT_EVAL_METHOD == 0`. The bit-exact regression tests in
  [`../../../test/`](../../test/) (`test_*_simd.c`, migrated through
  the [`simd_bitexact_test.h`](../../test/simd_bitexact_test.h) harness
  per ADR-0245) catch ULP drift; pushing through them without a
  paired scalar update is a regression.
- **No FMA on the load-bearing reductions.** `#pragma STDC FP_CONTRACT
  OFF` is set at TU level on every kernel that participates in
  ADR-0138 (`iqa_convolve` widen-then-add) or ADR-0139 (SSIM
  per-lane scalar-double accumulate). The compiler's default
  `-ffp-contract=fast` would silently fuse `a + b * c` and break
  bit-identity vs scalar.
- **Exception: SSIMULACRA 2 `picture_to_linear_rgb` colour matrix
  is unified ON FMA across all implementations** (ADR-0891). The
  AVX2 / AVX-512 main loops use `_mm256_fmadd_ps` / `_mm512_fmadd_ps`
  and the scalar tails + the `test_ssimulacra2_simd.c` reference
  use `fmaf()`. Reason: under icx + `-mfma`, the prior explicit
  `_mm256_add_ps(_, _mm256_mul_ps(_, _))` pattern was being
  auto-fused to FMA despite `-fp-model=precise`, while gcc kept
  it as separate mul+add; unifying on FMA on both sides is the
  cross-compiler bit-exact pairing. The left-to-right
  associativity of `G = Yn + cb_g*Un + cr_g*Vn` is preserved by
  chaining two FMAs (`G = fmaf(cb_g, Un, Yn);
  G = fmaf(cr_g, Vn, G);`). Do NOT revert to separate mul+add
  on rebase — the test will fail under icx.
- **Reserved-identifier hygiene** (ADR-0148): no leading-underscore
  names. The IQA tree underwent a sweeping `_iqa_*` →
  `iqa_*` / `_kernel` → `iqa_kernel` / `_ssim_int` →
  `ssim_int` rename; do not reintroduce the old spellings on
  rebase.

## Twin-update rules

These TUs come in twin-bundles. A change to one half **must** ship
with the matching change to the other halves in the **same PR**:

| Group | TUs that move in lockstep |
| --- | --- |
| **SSIM accumulate** (ADR-0139) | `ssim_avx2.c` + `ssim_avx512.c` + `../arm64/ssim_neon.c` + scalar `../iqa/ssim_tools.c` (`ssim_accumulate_default_scalar`) + shared helper `../iqa/ssim_accumulate_lane.h` |
| **IQA convolve** (ADR-0138 + ADR-0143) | `convolve_avx2.c` + `convolve_avx512.c` + `../arm64/convolve_neon.c` + scalar `../iqa/convolve.c` + shared scanline helpers `../common/convolution_avx.c` |
| **MS-SSIM decimate LPF** (ADR-0125) | `ms_ssim_decimate_avx2.c` + `ms_ssim_decimate_avx512.c` + `../arm64/ms_ssim_decimate_neon.c` + scalar `../ms_ssim_decimate.c`. The 9-tap filter table appears verbatim in all four — diff all four when any one moves. |
| **PSNR-HVS DCT** (ADR-0159 + ADR-0350) | `psnr_hvs_avx2.c` + `../arm64/psnr_hvs_neon.c` + scalar `../third_party/xiph/psnr_hvs.c`. Butterfly block is byte-identical across the three. **No `psnr_hvs_avx512.c` — AVX-512 closed as ceiling under T3-9 (a) per [ADR-0350](../../../../docs/adr/0350-psnr-hvs-avx512-ceiling.md): `perf record` cycle share is 78.42 % scalar tail (locked by ADR-0138/0139 bit-exactness) vs 14.82 % DCT, capping a 16-lane widening at 1.07–1.08× over AVX2 (Amdahl ceiling 1.17×) — well below T3-9's 1.3× ship gate.** Re-bench gate: any future upstream change to the Xiph/Daala scalar that shifts the per-block summation tree requires re-running [Research-0091 §7](../../../../docs/research/0091-psnr-hvs-avx512-bench-2026-05-09.md) before claiming the ceiling still holds. |
| **VIF SIMD8** (ADR-0146) | `vif_statistic_avx2.c` (`vif_stat_simd8_compute` + `vif_stat_simd8_reduce` halves around `struct vif_simd8_lane`) + scalar `../vif.c`. Per-lane scalar-float reduction via 32-byte aligned `tmp_n[8]` / `tmp_d[8]` is load-bearing for ADR-0139. |
| **CAMBI calculate_c_values_row** (ADR-0452) | `cambi_avx2.c` (`calculate_c_values_row_avx2`) + `cambi_avx512.c` (`calculate_c_values_row_avx512`) + `../arm64/cambi_neon.c` (`calculate_c_values_row_neon`) + scalar in `../cambi.c` (`calculate_c_values_row`). Every cambi inner-loop function ported to AVX2 **must** have AVX-512 + NEON siblings in the **same PR**. Bit-exact (integer pipeline, no float reduction tree). Tested in `../../test/test_cambi_simd.c`. |
| **Integer ADM p-norm callback ABI** (ADR-0645) | `adm_avx2.c` + `adm_avx512.c` + scalar `../integer_adm.c` + headers `adm_avx2.h` / `adm_avx512.h`. The `adm_cm` and `i4_adm_cm` signatures must carry `adm_p_norm` through every twin so `integer_adm:adm_p_norm=...` is not silently ignored by x86 SIMD dispatch. Default `3.0` expression shape remains the Netflix-compatible path. |
| **SSIMULACRA 2 SIMD** (ADR-0161 / 0162 / 0163 / 0252) | `ssimulacra2_avx2.c` + `ssimulacra2_avx512.c` + `../arm64/ssimulacra2_neon.c` + `../arm64/ssimulacra2_sve2.c` + `ssimulacra2_host_avx2.c` + `../arm64/ssimulacra2_host_neon.c` + scalar `../ssimulacra2.c` + Vulkan host-path call site `../vulkan/ssimulacra2_vulkan.c` |
| **float_moment SIMD** (ADR-0179 / ADR-0987) | `moment_avx2.c` + `moment_avx512.c` + `../arm64/moment_neon.c` + `../arm64/moment_sve2.c` + scalar `../float_moment.c`. Pure reduction kernels — no inter-pixel dependence, so bit-exactness contract is tolerance-bounded (1e-7 relative, not byte-exact); tested in `../../test/test_moment_simd.c`. The AVX-512 path (`HAVE_AVX512` gate, `compute_1st/2nd_moment_avx512`) widens the 8-lane AVX2 path to 16-lane ZMM. Do NOT change the sequential per-lane `double` accumulation order without updating the tolerance and the parity tests. |
| **ADM decouple LUT prefetch** (ADR-0502) | `adm_avx512.c` (`adm_decouple_avx512`, lines 956–968). The 16-iteration software-prefetch block before the `vpgatherdd` cluster must stay at distance 2 iterations (j+32). If the arithmetic body between prefetch and gather shrinks below ~100 cycles, increase to 3 iterations (j+48); if it grows above ~500 cycles, decrease to 1 (j+16). The prefetch target is `_MM_HINT_T1` (L2) not `_MM_HINT_T0` (L1) — the immediately following band-buffer loads would evict L1 lines before the gather executes. **Do not convert to `_MM_HINT_T0`; do not inline into a `_mm512_prefetch_i32gather_ps` — the latter requires `<zmmintrin.h>` and is not portable across all AVX-512 toolchains.** |
| **`vif_subsample_rd_8` noinline helpers** (ADR-0503) | `vif_avx512.c` (`vif_subsample_rd_8_vert_j` + `vif_subsample_rd_8_horiz_j`). These are `static __attribute__((noinline))` helpers carved from `vif_subsample_rd_8_avx512` to eliminate a ~30-ZMM live-set spill cluster. **Do NOT mark them `inline`, `always_inline`, or remove `noinline` — doing so re-merges the vertical and horizontal register live-sets back into the caller frame and restores the spill cluster.** Any change to the accumulation order inside these helpers breaks ADR-0138 / 0139 bit-exactness. |
| **Motion v2 NEON / AVX2 divergence** (ADR-0145) | `motion_v2_avx2.c` (currently uses `_mm256_srlv_epi64` *logical*) is **knowingly out-of-spec** vs scalar; `../arm64/motion_v2_neon.c` matches scalar via arithmetic shift. Do NOT port the AVX2 logical pattern to NEON. The AVX2 audit is a separate batch. |
| **Speed_chroma covariance-sum SIMD dispatch** (upstream 30f472b14, 2026-06-03) | `speed_avx2.c` + `speed_avx512.c` + scalar `compute_cov_kernel_scalar` in `../speed.c`. The three kernels share the `compute_cov_kernel_fn` typedef declared in `speed.c` and dispatched via `SpeedState::compute_cov_kernel` (set in `speed_init`). Any change to the kernel signature or the `SpeedState` struct must propagate to all three. Tolerance contract: 1e-9 relative (not byte-exact) due to FMA rounding; tested in `../../test/test_speed_simd.c`. **No NEON path yet** — the scalar kernel is always selected on non-x86 hosts. |

The complete invariants live in [../AGENTS.md
§"Rebase-sensitive invariants"](../AGENTS.md); this table is the
**index** of which file groups move together.

## simd_dx macros (ADR-0140)

[`../simd_dx.h`](../simd_dx.h) is fork-internal. AVX2 / AVX-512 paths
in this directory consume `SIMD_WIDEN_ADD_F32_F64_AVX2` /
`SIMD_WIDEN_ADD_F32_F64_AVX512`, `SIMD_ALIGNED_F32_BUF_*`,
`SIMD_LANES_*` to encode the ADR-0138 / 0139 patterns by
construction. Macro names are ISA-suffixed on purpose; do not
collapse them into cross-ISA aliases — the fork's SIMD policy
rules out Highway / simde / xsimd (user memory
`feedback_simd_dx_scope.md`).

## Adding a new AVX2 / AVX-512 TU

Use [`/add-simd-path`](../../../../.claude/skills/add-simd-path/SKILL.md).
The skill scaffolds:

1. The TU + header, with `#pragma STDC FP_CONTRACT OFF` at the
   top and the appropriate `#include "../simd_dx.h"`.
2. The dispatch entry in the feature's `*_dispatch.c` so
   `vmaf_get_cpu_flags_x86()` selects the new path.
3. A bit-exact regression test under `../../test/test_<feature>_simd.c`
   using the [`simd_bitexact_test.h`](../../test/simd_bitexact_test.h)
   harness (ADR-0245).

## Upstream-sync notes

- Every TU in this directory carries a Netflix copyright header
  (`Copyright 2016-202x Netflix, Inc.`) — these files are
  upstream-mirror at the structural level even though several
  carry fork-only refactors (ADR-0146 helper splits in
  `vif_statistic_avx2.c`; ADR-0143 `static` + `ptrdiff_t` in
  `convolve_avx2.c`).
- On `/sync-upstream` or `/port-upstream-commit`: if a Netflix
  patch touches any TU in this directory, walk the corresponding
  twin in `../arm64/` + the scalar reference + the SIMD-tail
  reduction helper (`../iqa/ssim_accumulate_lane.h` for SSIM,
  `../iqa/convolve.c` for convolve) before merging. The cross-
  backend parity gate at `places=4`
  ([`scripts/ci/cross_backend_parity_gate.py`](../../../../scripts/ci/cross_backend_parity_gate.py),
  ADR-0214) catches scalar↔SIMD drift but only after a full run.
- VIF kernelscale stays on the precomputed `vif_filter1d_table_s`
  flow ([Research-0024 Strategy E](../../../../docs/research/0024-vif-upstream-divergence.md)).
  Do **not** port Netflix `4ad6e0ea` / `8c645ce3` runtime helpers
  verbatim — they lose the bit-exact contract that ADR-0138 /
  0139 / 0142 / 0143 froze.

## Governing ADRs

See [../AGENTS.md §Governing ADRs](../AGENTS.md) for the full list.
The ones that carve invariants on this directory specifically:

- [ADR-0125](../../../../docs/adr/0125-ms-ssim-decimate-simd.md) —
  MS-SSIM decimate separable SIMD.
- [ADR-0138](../../../../docs/adr/0138-iqa-convolve-avx2-bitexact-double.md) —
  `iqa_convolve` widen-then-add bit-exactness.
- [ADR-0139](../../../../docs/adr/0139-ssim-simd-bitexact-double.md) —
  SSIM accumulate per-lane scalar-double reduction.
- [ADR-0140](../../../../docs/adr/0140-simd-dx-framework.md) —
  `simd_dx.h` framework.
- ADR-0143
  ([`0143-port-netflix-f3a628b4-generalized-avx-convolve.md`](../../../../docs/adr/0143-port-netflix-f3a628b4-generalized-avx-convolve.md))
  — generalised AVX convolve scanlines.
- [ADR-0146](../../../../docs/adr/0146-nolint-sweep-function-size.md) —
  IQA / VIF SIMD helper decomposition.
- [ADR-0148](../../../../docs/adr/0148-iqa-rename-and-cleanup.md) —
  reserved-identifier rename.
- [ADR-0159](../../../../docs/adr/0159-psnr-hvs-avx2-bitexact.md) —
  `psnr_hvs` AVX2 DCT.
- [ADR-0161](../../../../docs/adr/0161-ssimulacra2-simd-bitexact.md) +
  [ADR-0162](../../../../docs/adr/0162-ssimulacra2-iir-blur-simd.md) +
  [ADR-0163](../../../../docs/adr/0163-ssimulacra2-ptlr-simd.md) +
  [ADR-0252](../../../../docs/adr/0252-ssimulacra2-host-xyb-simd.md) —
  SSIMULACRA 2 SIMD ports.
- [ADR-0245](../../../../docs/adr/0245-simd-bitexact-test-harness.md) —
  shared bit-exact test harness.
- [ADR-0784](../../../../docs/adr/0784-integer-ssim-avx2.md) —
  integer SSIM AVX2 horizontal moment accumulation.

## integer_ssim_avx2 — rebase-sensitive invariant (ADR-0784)

`integer_ssim_avx2.c` exports `integer_ssim_accumulate_row_avx2` (8bpc)
and `integer_ssim_accumulate_row_16_avx2` (16bpc), dispatched from
`integer_ssim.c::init()` via function pointers in `IntegerSsimState`.
The layout of `integer_ssim_moments_t` (six consecutive `int64_t` fields
in the same order as `ssim_moments`) is a cross-TU invariant: changing
field order or inserting padding breaks the cast in `calc_ssim()`.
Any upstream change to `ssim_moments` in `integer_ssim.c` must be
mirrored in `integer_ssim_avx2.h`.

- [ADR-0918](../../../../docs/adr/0918-llvm-ir-diff-harness.md) —
  LLVM IR diff harness. **Rebase-sensitive invariant**: any compiler
  bump (`dev/Containerfile` clang version, GitHub Actions runner image,
  `.github/workflows/*.yml` clang install lines) MUST be accompanied by
  a local `make ir-diff` run. If the snapshots under
  `testdata/ir-snapshots/` drift, do not regenerate them blindly —
  investigate which intrinsic / FMA / FP-contract behaviour changed
  and confirm it does not break the bit-exact contract that ADRs 0125
  / 0138 / 0139 froze. Only after confirming intent is preserved (or
  the ADRs are updated) is `make ir-diff-update` appropriate, with
  justification in the commit message.
