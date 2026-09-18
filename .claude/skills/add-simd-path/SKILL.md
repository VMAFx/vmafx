---
name: add-simd-path
description: Scaffold a new SIMD implementation for an existing feature. Creates intrinsics source + header + a bit-exact-vs-scalar comparison test; wires into runtime dispatch. Supports kernel-spec flags that pull recurring patterns from simd_dx.h.
---
<!-- markdownlint-disable MD060 -->

# /add-simd-path

## Invocation

```text
/add-simd-path <isa> <feature> [--kernel-spec=<spec>] [--lanes=N] [--tail=scalar|masked]
```

- `<isa>` ∈ `avx2`, `avx512`, `avx512icl`, `neon`.
- `<feature>` ∈ feature names under `core/src/feature/` (`vif`, `adm`,
  `motion`, `ciede`, `ssim`, `convolve`, `ssimulacra2`).
- `--kernel-spec=<spec>` (optional):
  - `widen-add-f32-f64` -> single-rounded `float * float` -> widen to double
    -> double add. ADR-0138 + `simd_dx.h` macros
    `SIMD_WIDEN_ADD_F32_F64_{AVX2_4L,AVX512_8L,NEON_4L}`.
  - `per-lane-scalar-double` -> float intermediates in SIMD, reduce per-lane
    in scalar double (matches scalar C `2.0 *` / `double = float_expr`
    promotions). ADR-0139 + `SIMD_ALIGNED_F32_BUF_*` helpers.
  - `none` (default) -> generic pass-through stub.
- `--lanes=N` -> override SIMD block size. Defaults: 4 for AVX2 / NEON
  F32 -> F64 widen, 8 for AVX-512. For `per-lane-scalar-double`: 8 (AVX2),
  16 (AVX-512), 4 (NEON).
- `--tail=scalar|masked` -> handle `n % lanes` remainder.
  `scalar` = plain loop (simplest); `masked` = `_mm_maskload_ps` /
  `vld1q_f32` + conditional store (keeps SIMD throughput on short rows).

## Files created

| Path                                                       | Purpose                         |
|------------------------------------------------------------|---------------------------------|
| `core/src/feature/x86/<feature>_<isa>.c`                | Intrinsics impl (or arm64/…)    |
| `core/src/feature/x86/<feature>_<isa>.h`                | Prototype + ISA guard           |
| `core/test/test_<feature>_<isa>_bitexact.c`             | Bit-exact vs scalar comparison  |

## Files patched

- `core/src/cpu.c` or `cpu.h` -> dispatch table entry if absent.
- `core/src/feature/<feature>.c` or dispatch tools file (`iqa/ssim_tools.c` for
  SSIM/convolve) -> select SIMD impl when `cpu_supports_<isa>()`.
- `core/src/meson.build` -> add `<feature>_<isa>.c` under matching
  `is_asm_enabled` / AVX-512 guard.
- `core/test/meson.build` -> register bit-exact test with
  `host_machine.cpu_family` filter + `platform_specific_cpu_objects`.

## Template behaviour

Intrinsics stub:

1. Include ISA header (`immintrin.h` / `arm_neon.h`).
2. Include `../simd_dx.h`, use macros matching `--kernel-spec`.
3. Load data via `loadu` (or masked variant for `--tail=masked`).
4. Run scalar-semantics body (pass-through on `--kernel-spec=none`;
   documented widen-add reduction for `widen-add-f32-f64`;
   documented aligned-buf + per-lane scalar loop for
   `per-lane-scalar-double`).
5. Store via `storeu`.

Compiles, passes bit-exact test by definition (pass-through) or construction
(spec'd cases). Author replaces stub body with actual intrinsics, keeps
DX macros in place.

## Guardrails

- Refuse if `core/src/feature/x86/<feature>_<isa>.c` (or `arm64/...`)
  already exists.
- Bit-exact test MUST pass before merge. `--kernel-spec=widen-add-f32-f64` and
  `per-lane-scalar-double` templates pin scalar-match invariant at
  template-bake time; `simd-reviewer` agent verifies no unintended FMA /
  lane-reordering crept in.
- Run `/build-vmaf --backend=cpu` at end to confirm compilation.
- If `<isa> == neon` and dev host is x86_64, note reminds to cross-compile +
  run under `qemu-aarch64-static`:

  ```text
  meson setup build-aarch64 \
    --cross-file=build-aux/aarch64-linux-gnu.ini \
    -Denable_cuda=false -Denable_sycl=false
  ninja -C build-aarch64
  qemu-aarch64-static -L /usr/aarch64-linux-gnu \
    build-aarch64/tools/vmaf --cpumask 255 [...] -o scalar.xml
  qemu-aarch64-static ... --cpumask 0 [...] -o neon.xml
  diff <(grep -v '<fyi fps' scalar.xml) <(grep -v '<fyi fps' neon.xml)
  ```

## References

- [ADR-0138](../../../docs/adr/0138-iqa-convolve-avx2-bitexact-double.md) ->
  widen-add bit-exact pattern.
- [ADR-0139](../../../docs/adr/0139-ssim-simd-bitexact-double.md) ->
  per-lane scalar double reduction pattern.
- [ADR-0140](../../../docs/adr/0140-simd-dx-framework.md) ->
  skill upgrade + `simd_dx.h`.
- [simd_dx.h](../../../core/src/feature/simd_dx.h) -> macros.
