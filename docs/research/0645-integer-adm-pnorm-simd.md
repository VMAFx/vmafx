# Research-0645: Integer ADM p-norm SIMD callback audit

- **Date**: 2026-05-20
- **Scope**: `integer_adm` CPU scalar, AVX2, and AVX-512 contrast-measure
  callback plumbing.
- **Related ADR**: [ADR-0645](../adr/0645-integer-adm-pnorm-simd.md)

## Finding

`integer_adm` exposed the public feature option `adm_p_norm`, but the active
CPU callback signatures for `adm_cm` and `i4_adm_cm` did not carry the value.
That made the option ineffective on the fixed-point contrast-measure
finalisation exponent in both the scalar callback and the x86 SIMD callbacks
selected by runtime dispatch.

The practical operator-facing bug was easiest to see from the x86 twins:
`adm_avx2.c` and `adm_avx512.c` computed the final terms with hard-coded
`powf(..., 1.0f / 3.0f)`. A caller setting `adm_p_norm=2.0` could still get
the default `3.0` exponent in the SIMD path.

## Files Audited

- `core/src/feature/integer_adm.c`
- `core/src/feature/x86/adm_avx2.c`
- `core/src/feature/x86/adm_avx512.c`
- `core/src/feature/x86/adm_avx2.h`
- `core/src/feature/x86/adm_avx512.h`
- `core/test/test_integer_adm_simd.c`

## Result

The fix threads `adm_p_norm` through the internal callback ABI and uses
`1.0f / (float)adm_p_norm` for the final p-norm exponent in scalar, AVX2, and
AVX-512. The default value remains `3.0`, so default scoring keeps the same
expression shape.

The regression test extends `test_integer_adm_simd` with two checks:

- scale-0 `adm_cm_avx2` returns a different finite value for p=2 vs p=3;
- scale-1 `i4_adm_cm_avx2` returns a different finite value for p=2 vs p=3.

## Verification

```bash
docker exec vmaf-dev-mcp bash -lc \
  'rm -rf /tmp/vmaf-cpu-pnorm-build &&
   meson setup /tmp/vmaf-cpu-pnorm-build /workspace/libvmaf \
     -Denable_cuda=false -Denable_sycl=false \
     -Denable_vulkan=disabled -Denable_dnn=disabled &&
   meson test -C /tmp/vmaf-cpu-pnorm-build test_integer_adm_simd --print-errorlogs'
```

Result: `test_integer_adm_simd` passed.
