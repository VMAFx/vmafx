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
  moment_sve2.{c,h}         # SVE2 path for float_moment (ADR-0584) — VLA, lanes added in raster order (ADR-1500)
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
- **Integer-ADM DWT2: one NEON path on every AArch64 platform
  ([ADR-1257](../../../../docs/adr/1257-retire-darwin-adm-dwt2-legacy-dispatch.md),
  supersedes ADR-1057 Apple wrapper).** `integer_adm.c` dispatches
  `adm_dwt2_8_neon()` on Apple + Linux alike. Never reintroduce
  platform-specific first-column rule or score offset.
- **`adm_dwt2_8_neon()` horizontal 8-wide loop stops at
  `half_w >= 2 ? half_w - 1 - ((half_w - 2) % 8) : 1`** (Netflix/vmaf
  `ea012e387`, adapted). Column 0 + every column from that bound on go through
  `adm_dwt2_8_neon_hpass_column()` and `ind_x`, which applies mirror. Without
  bound: vector store runs past half-resolution row and, on last row, into next
  band of ADM slab. `test_adm_dwt2_neon` checks bit-exactness + guard band at
  production band stride.
- **`adm_decouple_neon()` returns scalar decouple's bits at every gain
  limit** (Netflix/vmaf `9e48141b`). Integral limit: four columns per step,
  int32 `rst * gain`; fractional limit or fewer than four columns:
  `adm_decouple_cols()` (`integer_adm_kernels.h`), ADR-1413 truncated
  double product. angle test is `adm_angle_flag_fp64()`'s double
  expression. Details and guards: [`../AGENTS.d/adm-rounding.md`](../AGENTS.d/adm-rounding.md).
- **Scale 1-3 DWT, scale 1-3 decouple and contrast masking of every scale
  return scalar's bits** (Netflix/vmaf `8bc5a5c6a`, `b41d2340a`, adapted).
  `adm_cm_neon()` / `i4_adm_cm_neon()` = interior-row callbacks of scalar
  drivers `adm_cm_rows()` / `i4_adm_cm_rows()` (row folded once, ADR-1167;
  scale-0 lanes uint64, scale 1-3 int64). Scale-0 centre tap int32, excess in
  int64 clamped to [0, INT32_MAX] (ADR-1402); upstream narrows tap to int16:
  never import that. Edge rows/cols, rows < 4 cols, leftover cols, i4 factor
  > INT32_MAX: scalar sample. `adm_decouple_s123_neon()`: vector path at gain
  limit 1 only, angle flag via scalar `adm_angle_flag()` for overshooting
  lanes; other limits scalar `adm_decouple_s123_cols()`.
  `adm_dwt2_s123_combined_neon()`: int64 taps + scale rounding + arithmetic
  shift, narrowed by truncation (`i4_dwt2_tap4()`); de-interleaved block only
  where `ind_x[k][j] = 2j - 1 + k` (j >= 1, 2j + 8 < w). Dispatch: cm only
  without `csf_requires_normalization`, as x86. Guards:
  `test_integer_adm_simd` (cm scale 0 + i4, decouple gains),
  `test_adm_dwt2_neon` (s123), GCC + clang under qemu.
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
  AArch64/QEMU.
- **`float_adm_dwt2_neon()` row helpers (ADR-1142, HISS-04).** Entry point
  keeps name + signature (dispatched from `adm.c`). Per row:
  `dwt2_vertical_row_neon()` (4-wide loop over `dwt2_vertical_4_neon()`,
  called for `flo` then `fhi`; scalar tail) and `dwt2_horizontal_row_neon()`
  (scalar, `ind_x`). Every function there carries GCC
  `optimize("-ffp-contract=off")` attribute: GCC's `vaddq_f32` / `vmulq_*` are
  plain `+` / `*`, so helper without it can fuse when built outside
  meson carve-out. New helper -> attribute, whole statements only, `accum`
  sequence unchanged. Vertical vector sum sits in its own helper because
  NEON intrinsic macros count as statements (`readability-function-size`,
  129 > 120 when inline).
- **Float-arithmetic NEON TUs belong in `arm64_v8_fp`** (not `arm64_v8`).
  Static lib `arm64_v8_fp` uses compiler-native no-contraction arguments
  from `vmaf_strict_fp_args` (ADR-0873); `arm64_v8` is integer-only and carries
  no FP flag. Adding float-arithmetic TU to `arm64_v8` is bit-exactness
  regression risk. Moving integer-only TU to `arm64_v8_fp` is harmless but
  unnecessary.
- **`accumulate_error()` and similar reductions thread accumulators by
  pointer** — do NOT introduce local-float accumulator inside helper.
  ADR-0159: local accumulator drifts Netflix golden by ~5.5e-5
  (`psnr_hvs_neon.c`).

- **SpEED covariance: `speed_neon.c` is row kernel of ADR-1459, not upstream's
  kernel.** `speed_cov_row_neon` pairs one x block with up to five y blocks at
  consecutive columns; lanes = separate covariance sums (two `float64x2_t` +
  one scalar), `vmulq_f64` then `vaddq_f64`, so every sum has bits of
  `compute_cov_kernel_scalar` (`../speed.c`; contract `../speed_cov.h`).
  `test_speed_simd` compares with `memcmp` under qemu, GCC + clang. Lives in
  `arm64_v8_fp` (no contraction); scalar fifth lane keeps product in own
  statement. Never `vfmaq_f64`, never one sum split over lanes: upstream
  `compute_cov_kernel_neon` (Netflix/vmaf 15297286) does both (4061 of 18480
  sums off, max relative 3.5e-12) and stays out on sync.
  `compute_cov_kernel_scalar` carries function-scoped no-contraction guard
  (ADR-1057 pattern): without it clang fused its loop remainder and no kernel
  could match at every width.

- **MSVC compiles this directory (ADR-1260, `Windows ARM64 MSVC` lane).**
  `cl.exe` ARM64: no GCC vector extensions on NEON types (`v[0]`, `a + b`,
  brace-initialised or compound-literal vectors), no `_x2`/`_x3`/`_x4`
  multi-register loads, `__attribute__` / `#pragma GCC` / `#pragma clang`
  only under `#if defined(__GNUC__)` or `defined(__clang__)` guards as
  today. `<arm_neon.h>` is include on every compiler. Strict FP comes from
  shared `vmaf_strict_fp_args` in `core/src/meson.build`: `/fp:precise` on
  MSVC, `/fp:precise /clang:-fno-fast-math /clang:-fcomplex-arithmetic=full
  /clang:-ffp-contract=off` on `intel-llvm-cl`,
  `/clang:-ffp-contract=off` on clang-cl, and `-ffp-contract=off` on GCC/clang.
  Never restore literal strict-FP option in arm64 `c_args`. SVE2 TUs
  never build under MSVC (no `<arm_sve.h>`; probe skipped). Local MSVC check
  is impossible; QEMU cross build covers GCC/clang side only.

## Twin-update rules

TUs come in twin-bundles. Change to one half **must** ship with matching change
to other halves in **same PR**:

| Group | TUs that move in lockstep |
| --- | --- |
| **SSIM accumulate** (ADR-0139) | `ssim_neon.c` + `../x86/ssim_avx2.c` + `../x86/ssim_avx512.c` + scalar `../iqa/ssim_tools.c` + shared helper `../iqa/ssim_accumulate_lane.h` |
| **IQA convolve** (ADR-0138 + ADR-0143) | `convolve_neon.c` + `../x86/convolve_avx2.c` + `../x86/convolve_avx512.c` + scalar `../iqa/convolve.c` |
| **MS-SSIM decimate LPF** (ADR-0125) | `ms_ssim_decimate_neon.c` + `../x86/ms_ssim_decimate_avx2.c` + `../x86/ms_ssim_decimate_avx512.c` + scalar `../ms_ssim_decimate.c`. 9-tap filter table appears verbatim in all four. |
| **PSNR-HVS** (ADR-0160, [ADR-1469](../../../../docs/adr/1469-psnr-hvs-simd-butterfly-two-stages.md)) | `psnr_hvs_neon.c` + `../x86/psnr_hvs_avx2.c` + scalar `../third_party/xiph/psnr_hvs.c`. Butterfly: 34 scalar statements, same text / order / names. Both SIMD twins: two functions, `od_bin_fdct8_even_simd()` (21 statements) + `od_bin_fdct8_odd_simd()` (13), cut at same statement; move cut in both or neither. Threading `ret` by pointer: load-bearing. `compute_masks()`: keep scalar `sqrt(mask * gvar) / 32` with `float` product (upstream's statement, ADR-1488); `double` product = one ulp off on ~1 block in 20. Gate: `test_psnr_hvs_dispatch_invariance` (whole path vs shipped scalar, bit compare, x86-64 + aarch64); run under `qemu-aarch64` after touching any of three. |
| **SSIMULACRA 2 SIMD** (ADR-0161 / 0162 / 0163 / 0213 / 0252) | `ssimulacra2_neon.c` + `ssimulacra2_sve2.c` + `../x86/ssimulacra2_avx2.c` + `../x86/ssimulacra2_avx512.c` + `ssimulacra2_host_neon.c` + `../x86/ssimulacra2_host_avx2.c` + scalar `../ssimulacra2.c` + Vulkan host-path `../vulkan/ssimulacra2_vulkan.c` |
| **CAMBI stage kernels** (ADR-1256, Research-2065) | `cambi_neon.c` + `../x86/cambi_avx2.c` (upstream mirror) + `../x86/cambi_avx512.c` + scalar `../cambi.c`; NEON / AVX-512 c-values drivers share walk `../cambi_c_values_frame.h`. Dispatched on NEON: anti-dither, derivative, decimate, dp row, c-values. Kept scalar: mask row, mode filter. Tests: `test_cambi_stage_simd.c`, `test_cambi_dispatch_invariance.c`, `test_cambi_simd.c` (run under `qemu-aarch64` without aarch64 host). |
| **CAMBI spatial-mask rows** (ADR-1256) | `cambi_neon.c` (`compute_dp_row_neon`, `compute_mask_row_neon`) + `../x86/cambi_avx2.c` + `../x86/cambi_avx512.c` twins + scalar reference in `../cambi.c`. Only dp row is dispatched on aarch64: GCC and Clang auto-vectorize scalar mask row into same `cmhi` / `uzp1` sequence, so `compute_mask_row_neon` stays built, parity-tested and undispatched — re-check compiled scalar before wiring it. dp row keeps single add on loop-carried chain; keep that shape. Tested in `../../test/test_cambi_spatial_mask_simd.c` (run under `qemu-aarch64` when no aarch64 host is available). |
| **float_moment SIMD** ([ADR-1500](../../../../docs/adr/1500-arm-float-moment-scalar-order.md)) | `moment_neon.c` + `moment_sve2.c` + `../x86/moment_avx2.c` + `../x86/moment_avx512.c` + scalar `../moment.c`. Every kernel squares in `float` (second moment) and adds each value into one `double` in raster order, one after other: scalar's bits on every input and SVE vector length. No lane accumulators, no per-row vector sums, no `svaddv` / `vaddvq_f64` reduction: past 2^53 units sum rounds on every add and any other grouping is another number. Gate: `test_moment_simd` (`==`) under `qemu-aarch64` with `sve=off`, `sve128`, `sve256`, `sve512` and `sve2048` after touching any of them. |
| **Motion v2 NEON** (ADR-0145) | `motion_v2_neon.c` uses **arithmetic** right-shift (`vshrq_n_s64(v, 16)` / `vshlq_s64(v, -(int64_t)bpc)`); matches scalar. Sister `../x86/motion_v2_avx2.c` uses `_mm256_srlv_epi64` (logical) — knowingly out-of-spec until AVX2 audit. **Do NOT port AVX2 logical pattern here.** 4-lane stride + scalar tails on both sides of row are load-bearing for x_conv edge-mirror contract. |

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
- `moment_sve2.c` — fully VLA: loads `svcntw()` samples under
  `svwhilelt_b32(j, w)`, stores active lanes (at most 64, 2048-bit
  maximum) and adds them into running `double` in lane order
  ([ADR-1500](../../../../docs/adr/1500-arm-float-moment-scalar-order.md)).
  active lanes of `whilelt` predicate are first ones, so adds
  are scalar's at every vector length. **On rebase: do not bring back
  vector widening and `svaddv_f64` per row** (vector-length-dependent
  grouping, not scalar's sum past 2^53 units).

Background for any future SVE kernel that widens f32 to f64 in vector:
`svcvt_f64_f32` (FCVT) reads source f32 element `2*i` into f64 element `i`,
not lower contiguous lanes; odd lanes need SVE2 `svcvtlt_f64_f32`
(FCVTLT). earlier `moment_sve2.c` widened with FCVT alone and summed
even lanes twice on registers wider than 64 bits.

SVE2 test cases of `test_*_simd.c` probe `vmaf_get_cpu_flags_arm()`.
`vmaf_get_cpu_flags()` reads 0 until `vmaf_init_cpu()` has run, so test
that asks it without calling `vmaf_init_cpu()` skips its cases on every
processor (`test_moment_simd` and `test_iqa_convolve` did until ADR-1500).

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

## SSIMULACRA 2 shared header (ADR-1142)

`ssimulacra2_arm64_common.h` = one definition of scalar parts NEON, SVE2,
host TUs share: XYB (`Ss2XybK`, `ss2_xyb_block_neon`, `ss2_xyb_pixel`), SSIM +
edge-diff sums, YUV scalar pixel (`Ss2YuvK`), NEON 2x2 row. Includers set
`#pragma STDC FP_CONTRACT OFF` before include.

- Rebase: scalar expression change in `ssimulacra2_math.h` /
  `ssimulacra2.c` changes header same PR; no per-TU copy back.
- SVE vector not struct member or array element: SVE2 helpers keep scalars
  or state rows in memory.
- Every kernel <= 60 lines (HISS-04); `NOLINT(readability-function-size)`
  carve-outs gone.
- Guard: `test_ssimulacra2_simd` (NEON + SVE2 vs scalar, `==`).

## Adding a new NEON / SVE2 TU

Use [`/add-simd-path`](../../../../.claude/skills/add-simd-path/SKILL.md).
Skill scaffolds TU + header + dispatch entry + bit-exact regression test using
shared [`simd_bitexact_test.h`](../../test/simd_bitexact_test.h) harness
(ADR-0245).

## Lint: this directory has exactly one lane (ADR-1283)

Only `arm64` clang-tidy ratchet lane measures these TUs. `cpu`, `cuda`,
`hip` and `sycl` lanes all build x86, so none of their compile databases holds
command for anything here, and `exclude_untidyable()` in
`.github/workflows/lint-and-format.yml` drops `^core/src/feature/arm64/` from
fast `Tidy Changed` job for same reason. Before lane existed
`vif_neon.c` carried 36 findings that nothing had ever reported.

Re-measure after any change here, and commit tightened baseline in same
PR (ADR-1142 — cleaned file whose baseline was not tightened fails `exit 3`):

```bash
meson setup build-arm64 core --cross-file build-aux/aarch64-linux-gnu.ini \
    -Denable_cuda=false -Denable_sycl=false -Db_lto=false
ninja -C build-arm64            # generated model-JSON -> C sources must exist
make tidy-ratchet LANE=arm64 TIDY_RATCHET_BUILD_DIR=build-arm64
```

Needs aarch64 cross gcc and glibc sysroot; clang-tidy is handed same
`--target` / `--sysroot` so it parses `<arm_neon.h>` and `<arm_sve.h>` as
AArch64 rather than against host's x86 headers.

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
