---
paths:
  - core/src/feature/arm64/cambi_neon.*
  - core/src/feature/arm64/*sve2*
  - core/src/feature/arm64/ssimulacra2*
invariant: CAMBI NEON scan/tail rules, SVE2 4-lane versus VLA raster reductions, and shared SSIMULACRA 2 ARM64 header.
---
# CAMBI NEON, SVE2 VLA rules, and SSIMULACRA 2 shared header

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
  ([ADR-1500](../../../../../docs/adr/1500-arm-float-moment-scalar-order.md)).
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
  (see [`../../arm/cpu.c`](../../../arm/cpu.c) +
  [`../../arm/AGENTS.md`](../../../arm/AGENTS.md)). On aarch64 hosts without
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
