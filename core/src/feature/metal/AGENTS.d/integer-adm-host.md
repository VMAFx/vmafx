---
paths:
  - core/src/feature/metal/integer_adm_metal_host.c
  - core/src/feature/metal/integer_adm_metal_host.h
  - core/src/feature/metal/metal_integer_adm_uniforms.h
  - core/src/feature/metal/integer_adm_metal.mm
invariant: integer_adm_metal: host logic, slots and scale-1 parent (ADR-1806).
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# `integer_adm_metal`: host logic, slots, scale-1 parent (ADR-1806)

T-METAL-INTEGER-ADM-TWIN-DEFECTS-2026-10-05: M4 Pro report (#2118) = every
exact case off. Six defects, each now one definition:

- **Uniforms + slots = `metal_integer_adm_uniforms.h`** (MSL + C). `IadmDims`,
  `IadmCsf` defined once; kernel and host include it. Reduction slot address
  only via `vmaf_mtl_iadm_accum_word()` (kernel wrote stride 36, host read 18:
  band d lost, out-of-bounds writes). No second struct copy in `.mm`/`.metal`.
- **Host logic = `integer_adm_metal_host.c`** (plain C, no Metal API): geometry,
  buffer sizes, stage plan (`iadm_metal_stages()`), uniforms, scores. `.mm` =
  alloc, bind, encode, emit only. Uniform shifts/rounding copied from CPU
  contexts (`adm_cm_ctx_init()`, `i4_adm_cm_ctx_init()`,
  `adm_csf_den_ctx_init()`, `i4_adm_csf_den_ctx_init()`, `i4_dwt2_round()`);
  scores = `adm_cm_result()` / `adm_csf_den_result()` + `i4_` forms, double
  noise weight. No local table, no local quant step, no local `powf`.
- **Scale 1 reads int16** (`integer_adm_dwt_vert_s1`, CPU `i16_to_i32()`);
  scales 2-3 int32 (`integer_adm_dwt_vert_s123`). Never bind int16 band to
  int kernel.
- **Scales 1-3 masking terms** = `vmaf_mtl_iadm_i4_masking_term()` with
  `I4AdmCmCtx::add_bef_shift_flt` = INT32_MIN (Netflix#955, ADR-0155), never
  +2^31. Denominator square add = `I4AdmDenCtx::add_shift_sq` = 2^shift_sq.
- **`adm_skip_scale0`**: scale 0 = DWT only, num 0, den 1e-10f, AIM 0.
- **`kernel` is macro in host shim**: no host-header identifier named
  `kernel` (stage field = `entry`); no MSL type names (`half`) in headers
  kernels include (`test_metal_shader_build_contract`).
- Guards: `test_metal_integer_adm_host_replay` (unmodified `.metal` through
  `core/test/metal_msl_host_shim.h`, `==` vs CPU, guard bands),
  `test_metal_integer_adm_math`, `test_metal_integer_adm_exact_contract.py`.
  Device: `test_metal_integer_adm_parity`. Replay limits: one thread per
  threadgroup (no barrier/race coverage), not Metal compiler.
