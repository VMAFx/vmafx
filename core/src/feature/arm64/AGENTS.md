# AGENTS.md — core/src/feature/arm64

Agent orientation: NEON / SVE2 feature SIMD paths.
Parent: [../AGENTS.md](../AGENTS.md). Sister directory:
[`../x86/`](../x86/AGENTS.md).

## Scope

Per-feature aarch64 NEON + SVE2 SIMD implementations.
Every TU mirrors scalar reference one level up; runtime dispatch from
feature `*_dispatch.c` via `vmaf_get_cpu_flags_arm()`
(see [`../../arm/cpu.c`](../../arm/cpu.c)).

```text
feature/arm64/
  <feature>_neon.{c,h}      # NEON path (aarch64 baseline; always available on ARCH_AARCH64)
  ssimulacra2_sve2.{c,h}    # SVE2 path (ADR-0213) — runtime-gated via HWCAP2_SVE2
  moment_sve2.{c,h}         # SVE2 path for float_moment (ADR-0584) — VLA f32→f64 reduction
  ms_ssim_decimate_neon.*   # 9-tap LPF SIMD (one of four byte-identical TUs — see parent AGENTS.md)
```

## Ground rules

- **Every SIMD `.h` file MUST be self-contained.** Include every standard
  header for types in own declarations; do not rely on transitive includes from
  consumer `.c` files. Header declaring `ptrdiff_t` parameter MUST include
  `<stddef.h>` directly. Standalone-include failures on Apple Clang and Ubuntu
  ARM Clang = CI regressions (see PR #914 for cambi family; fixed for motion
  family in accompanying PR).
- **Parent rules** apply in full (see [../AGENTS.md](../AGENTS.md) +
  [../../AGENTS.md](../../AGENTS.md)).
- **Bit-exactness with scalar reference is non-negotiable.** Same rule as AVX2 /
  AVX-512 sibling: every NEON kernel mirrors scalar TU byte-for-byte under
  `FLT_EVAL_METHOD == 0`. Bit-exact regression tests in
  [`../../../test/`](../../test/) (`test_*_simd.c`, migrated through
  [`simd_bitexact_test.h`](../../test/simd_bitexact_test.h) harness per
  ADR-0245) catch ULP drift.
- **Integer-ADM has one named Apple production compatibility boundary
  (ADR-1057, 2026-08-31).** `adm_dwt2_8_neon()` remains universal four-tap,
  scalar-bit-exact kernel; `test_adm_dwt2_neon` must keep proving that on every
  AArch64 platform. On Apple AArch64 only, `integer_adm.c` dispatches
  production calls through `adm_dwt2_8_neon_apple_legacy()`. Wrapper overwrites
  only first output column with historical three-tap boundary result from
  immutable Darwin Python goldens. Do not move three-tap rule back into
  universal kernel, broaden to Linux AArch64, or replace with score offset.
  Same test separately proves compatibility wrapper matches legacy reference
  and changes no column except `j == 0`.
- **`#pragma STDC FP_CONTRACT OFF` kept at TU level** even though aarch64 GCC
  ignores it with non-fatal `-Wunknown-pragmas`. Pragma is portable; aarch64 GCC
  does not contract `a + b * c` across statements at default optimisation
  anyway. Removal on rebase loses cross-architecture documentation.
- **Float-ADM DWT2 unconditionally non-contracting (ADR-1057, 2026-08-31).**
  Golden-producing scalar `adm_dwt2_s` carries function-scoped Clang
  `contract(off)` pragma and GCC `optimize("-ffp-contract=off")` attribute.
  Do not widen either guard to all of `adm_tools.c`: earlier file-scope form
  changed unrelated ADM reductions. `float_adm_dwt2_neon.c` starts every
  accumulator at +0, then uses four explicit `vmulq_laneq_f32` + `vaddq_f32`
  steps and matching scalar sequences. Initial +0 load-bearing for
  signed-zero parity with `adm_dwt2_s`. Keep NEON TU `-ffp-contract=off`;
  do not introduce `vfmaq`, `fmaf`, or other fused form. Guarded by bit-exact
  `test_float_adm_dwt2_neon` (including signed zero) under Clang and GCC
- **Bit-exactness with the scalar reference is non-negotiable.** Same
  rule as the AVX2 / AVX-512 sibling — every NEON kernel mirrors the
  scalar TU byte-for-byte under `FLT_EVAL_METHOD == 0`. The bit-exact
  regression tests in [`../../../test/`](../../test/) (`test_*_simd.c`,
  migrated through the [`simd_bitexact_test.h`](../../test/simd_bitexact_test.h)
  harness per ADR-0245) catch ULP drift.
- **Integer-ADM DWT2 has one NEON path on every AArch64 platform
  ([ADR-1257](../../../../docs/adr/1257-retire-darwin-adm-dwt2-legacy-dispatch.md),
  which supersedes ADR-1057's Apple wrapper).** `integer_adm.c` dispatches
  `adm_dwt2_8_neon()` on Apple and Linux alike. Do not reintroduce a
  platform-specific first-column rule or a score offset.
- **`adm_dwt2_8_neon()`'s horizontal 8-wide loop stops at
  `half_w >= 2 ? half_w - 1 - ((half_w - 2) % 8) : 1`** (Netflix/vmaf
  `ea012e387`, adapted). Column 0 and every column from that bound on go
  through `adm_dwt2_8_neon_hpass_column()` and `ind_x`, which applies the
  mirror. Without the bound the vector store runs past the half-resolution
  row and, on the last row, into the next band of the ADM slab.
  `test_adm_dwt2_neon` checks both bit-exactness and a guard band at the
  production band stride.
- **`#pragma STDC FP_CONTRACT OFF` is kept at TU level** even though
  aarch64 GCC ignores it with a non-fatal `-Wunknown-pragmas`. The
  pragma is portable and aarch64 GCC does not contract `a + b * c`
  across statements at default optimisation anyway. Removing it on
  rebase loses the cross-architecture documentation.
- **Float-ADM DWT2 is unconditionally non-contracting (ADR-1057,
  2026-08-31).** The golden-producing scalar `adm_dwt2_s` carries a
  function-scoped Clang `contract(off)` pragma and GCC
  `optimize("-ffp-contract=off")` attribute. Do not widen either guard to
  all of `adm_tools.c`: the earlier file-scope form changed unrelated ADM
  reductions. `float_adm_dwt2_neon.c` starts every accumulator at +0,
  then uses four explicit `vmulq_laneq_f32` + `vaddq_f32` steps and
  matching scalar sequences. The initial +0 is load-bearing for signed-zero
  parity with `adm_dwt2_s`. Keep the NEON TU's `-ffp-contract=off`; do not
  introduce `vfmaq`, `fmaf`, or another fused form. Guarded by bit-exact
  `test_float_adm_dwt2_neon` (including signed zero) under both Clang and GCC
  AArch64/QEMU.
- **Float-arithmetic NEON TUs belong in `arm64_v8_fp`** (not `arm64_v8`).
  Static lib `arm64_v8_fp` compiled with `-ffp-contract=off` (ADR-0873);
  `arm64_v8` is integer-only, carries no FP flag. Adding float-arithmetic TU to
  `arm64_v8` = bit-exactness regression risk. Moving integer-only TU to
  `arm64_v8_fp` harmless but unnecessary.
- **`accumulate_error()` and similar reductions thread accumulators by
  pointer** — do NOT introduce local-float accumulator inside helper.
  ADR-0159: local accumulator drifts Netflix golden by ~5.5e-5
  (`psnr_hvs_neon.c`).

- **MSVC compiles this directory (ADR-1260, `Windows ARM64 MSVC` lane).**
  `cl.exe` ARM64: no GCC vector extensions on NEON types (`v[0]`, `a + b`,
  brace-initialised or compound-literal vectors), no `_x2`/`_x3`/`_x4`
  multi-register loads, `__attribute__` / `#pragma GCC` / `#pragma clang`
  only under `#if defined(__GNUC__)` or `defined(__clang__)` guards as
  today. `<arm_neon.h>` is the include on every compiler. Strict FP:
  `arm64_strict_fp_args` in `core/src/meson.build` (`/fp:precise` on msvc,
  `-ffp-contract=off` else); never a literal `-ffp-contract=off` in the
  arm64 `c_args`. SVE2 TUs never build under MSVC (no `<arm_sve.h>`;
  probe skipped). Local MSVC check impossible; qemu cross build covers
  GCC/clang side only.

## Twin-update rules

TUs come in twin-bundles. Change to one half **must** ship with matching change
to other halves in **same PR**:

| Group | TUs that move in lockstep |
| --- | --- |
| **SSIM accumulate** (ADR-0139) | `ssim_neon.c` + `../x86/ssim_avx2.c` + `../x86/ssim_avx512.c` + scalar `../iqa/ssim_tools.c` + shared helper `../iqa/ssim_accumulate_lane.h` |
| **IQA convolve** (ADR-0138 + ADR-0143) | `convolve_neon.c` + `../x86/convolve_avx2.c` + `../x86/convolve_avx512.c` + scalar `../iqa/convolve.c` |
| **MS-SSIM decimate LPF** (ADR-0125) | `ms_ssim_decimate_neon.c` + `../x86/ms_ssim_decimate_avx2.c` + `../x86/ms_ssim_decimate_avx512.c` + scalar `../ms_ssim_decimate.c`. The 9-tap filter table appears verbatim in all four. |
| **PSNR-HVS DCT** (ADR-0160) | `psnr_hvs_neon.c` + `../x86/psnr_hvs_avx2.c` + scalar `../third_party/xiph/psnr_hvs.c`. Butterfly block byte-identical across the three; threading `ret` by pointer is load-bearing. |
| **SSIMULACRA 2 SIMD** (ADR-0161 / 0162 / 0163 / 0213 / 0252) | `ssimulacra2_neon.c` + `ssimulacra2_sve2.c` + `../x86/ssimulacra2_avx2.c` + `../x86/ssimulacra2_avx512.c` + `ssimulacra2_host_neon.c` + `../x86/ssimulacra2_host_avx2.c` + scalar `../ssimulacra2.c` + Vulkan host-path `../vulkan/ssimulacra2_vulkan.c` |
| **CAMBI stage kernels** (ADR-1256, Research-2065) | `cambi_neon.c` + `../x86/cambi_avx2.c` (upstream mirror) + `../x86/cambi_avx512.c` + scalar `../cambi.c`; NEON / AVX-512 c-values drivers share walk `../cambi_c_values_frame.h`. Dispatched on NEON: anti-dither, derivative, decimate, dp row, c-values. Kept scalar: mask row, mode filter. Tests: `test_cambi_stage_simd.c`, `test_cambi_dispatch_invariance.c`, `test_cambi_simd.c` (run under `qemu-aarch64` without aarch64 host). |
| **CAMBI spatial-mask rows** (ADR-1256) | `cambi_neon.c` (`compute_dp_row_neon`, `compute_mask_row_neon`) + `../x86/cambi_avx2.c` + `../x86/cambi_avx512.c` twins + scalar reference in `../cambi.c`. Only the dp row is dispatched on aarch64: GCC and Clang auto-vectorize the scalar mask row into the same `cmhi` / `uzp1` sequence, so `compute_mask_row_neon` stays built, parity-tested and undispatched — re-check the compiled scalar before wiring it. The dp row keeps a single add on the loop-carried chain; keep that shape. Tested in `../../test/test_cambi_spatial_mask_simd.c` (run under `qemu-aarch64` when no aarch64 host is available). |
| **Motion v2 NEON** (ADR-0145) | `motion_v2_neon.c` uses **arithmetic** right-shift (`vshrq_n_s64(v, 16)` / `vshlq_s64(v, -(int64_t)bpc)`); matches scalar. Sister `../x86/motion_v2_avx2.c` uses `_mm256_srlv_epi64` (logical) — knowingly out-of-spec until the AVX2 audit. **Do NOT port the AVX2 logical pattern here.** 4-lane stride + scalar tails on both sides of the row are load-bearing for the x_conv edge-mirror contract. |

Complete invariants in [../AGENTS.md
§"Rebase-sensitive invariants"](../AGENTS.md).

## CAMBI NEON invariants (Research-2065)

- `filter_mode_neon`, `compute_mask_row_neon`: built, parity-tested, not
  dispatched. GCC + Clang vectorise scalar loop same way (insn count 0.93x /
  1.02x). Re-count with qemu insn plugin before wiring.
- NEON c-values driver uses plain C range updaters on purpose: compilers emit
  same 8-lane adds; intrinsic versions cost +0.3–0.9 % insns, retired. No
  `cambi_*_range_neon`.
- Scans: no masked load → scalar tail < 8 cols via shared
  `cambi_column_*` predicates (`../cambi_c_values_frame.h`, also AVX2). Never
  vector-load past last column (last row may end at buffer end).
- Scan may over-flag, never under-flag; mirrors `uh_slide` skip + band test.
- `cambi_neon.c` lives in integer lib `arm64_v8` (no `-ffp-contract=off`):
  c-value is one mul, no add, so nothing to fuse. Adding `a * b + c` float math
  here → move TU to `arm64_v8_fp`.

## SVE2 invariants (ADR-0213, ADR-0584)

`ssimulacra2_sve2.c` and `moment_sve2.c` = SVE2 consumers in directory.
Different VLA strategies:

- `ssimulacra2_sve2.c` — locked to fixed 4-lane predicate
  (`svwhilelt_b32(0, 4)`) for ADR-0161 byte-identity.
- `moment_sve2.c` — fully VLA: steps by `svcntw()` (full f32 register) per
  iteration, widening **both** lane halves to f64. Wider registers give
  throughput benefit at Neoverse V2 / Cortex-X4 widths.

**CRITICAL — SVE `FCVT .s→.d` lane mapping (`moment_sve2.c`).** SVE
`svcvt_f64_f32` (FCVT) does **not** compact lower contiguous f32 lanes into
f64 lanes. Destination f64 element `i` reads source f32 element `2*i`
(even-indexed lane in low half of 64-bit container, per ARM A64 reference).
Odd (top-half) f32 lanes read only by SVE2 `svcvtlt_f64_f32` (FCVTLT),
mapping f64 element `i` to f32 element `2*i+1`. Earlier `moment_sve2.c`
stepped by `svcntd()` and used only `svcvt_f64_f32_x` (assumed contiguous
lower-lane widening): on SVE register >64 bits, silently summed even f32
lanes x2 and dropped odd lanes (qemu relative error ~45% at 128-bit VL;
caught by `test_moment_simd.c` SVE2 under emulation, never by x86 CI).
Correct VLA pattern processes full `svcntw()` register, widens even lanes via
`svcvt_f64_f32_x` + odd lanes via `svcvtlt_f64_f32_x`, accumulates with
merging adds (`svadd_f64_m`, not `_x`, so partial-tail iteration cannot feed
undefined inactive lanes into reduction). **On rebase: do not revert to
single-FCVT `svcntd()` step.** FCVTLT requires FEAT_SVE2 (TU build gate).

`ssimulacra2_sve2.c` is **not** free perf knob:

- Kernel locked to fixed 4-lane predicate (`svwhilelt_b32(0, 4)`); arithmetic
  order matches NEON sibling regardless of runtime vector length. Widening to
  `svptrue_b32()` exposes lane-count drift across SVE2 hardware generations
  and breaks ADR-0161 bit-identity.
- Build develops against `qemu-aarch64-static`; CI runs SVE2 smoke under
  qemu. Real-hardware verification opportunistic (no SVE2 self-hosted runner
  yet).
- Runtime gate: `vmaf_get_cpu_flags_arm()` sets `VMAF_ARM_CPU_FLAG_SVE2` bit
  only when kernel `AT_HWCAP2` reports `HWCAP2_SVE2`
  (see [`../../arm/cpu.c`](../../arm/cpu.c) +
  [`../../arm/AGENTS.md`](../../arm/AGENTS.md)). On aarch64 hosts without
  SVE2, dispatcher falls back to NEON automatically.

**On rebase**: do not widen predicate, do not re-order matmul / downsample
chain, do not introduce vector `cbrtf` / `powf` polynomials. SSIMULACRA 2
invariants apply identically to NEON and SVE2.

## Adding a new NEON / SVE2 TU

Use [`/add-simd-path`](../../../../.claude/skills/add-simd-path/SKILL.md).
Skill scaffolds TU + header + dispatch entry + bit-exact regression test using
shared [`simd_bitexact_test.h`](../../test/simd_bitexact_test.h) harness
(ADR-0245).

## Upstream-sync notes

Same rules as [`../x86/AGENTS.md`](../x86/AGENTS.md): every TU carries Netflix
copyright header at structural level. On `/sync-upstream` walk AVX twin +
scalar reference + shared SIMD-tail reduction helper before merging.
Cross-backend parity gate at `places=4` catches drift only after full run.

## Governing ADRs

Full list: [../AGENTS.md §Governing ADRs](../AGENTS.md).
Directory invariants:

- [ADR-0125](../../../../docs/adr/0125-ms-ssim-decimate-simd.md) — MS-SSIM
  decimate separable SIMD.
- [ADR-0139](../../../../docs/adr/0139-ssim-simd-bitexact-double.md) — SSIM
  accumulate per-lane scalar-double reduction.
- [ADR-0140](../../../../docs/adr/0140-simd-dx-framework.md) — `simd_dx.h`
  framework.
- [ADR-0145](../../../../docs/adr/0145-motion-v2-neon-bitexact.md) — `motion_v2`
  NEON arithmetic-shift contract.
- [ADR-0160](../../../../docs/adr/0160-psnr-hvs-neon-bitexact.md) — `psnr_hvs`
  NEON DCT.
- [ADR-0161](../../../../docs/adr/0161-ssimulacra2-simd-bitexact.md) +
  [ADR-0162](../../../../docs/adr/0162-ssimulacra2-iir-blur-simd.md) +
  [ADR-0163](../../../../docs/adr/0163-ssimulacra2-ptlr-simd.md) +
  [ADR-0213](../../../../docs/adr/0213-ssimulacra2-sve2.md) +
  [ADR-0252](../../../../docs/adr/0252-ssimulacra2-host-xyb-simd.md) —
  SSIMULACRA 2 SIMD ports (NEON + SVE2 + host-path).
- [ADR-0245](../../../../docs/adr/0245-simd-bitexact-test-harness.md) — shared
  bit-exact test harness.
- [ADR-0584](../../../../docs/adr/0584-moment-sve2-port.md) — `float_moment`
  SVE2 VLA f32→f64 port.
