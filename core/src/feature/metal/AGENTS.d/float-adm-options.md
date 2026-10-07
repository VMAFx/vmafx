---
paths:
  - core/src/feature/metal/float_adm_metal.mm
  - core/src/feature/metal/float_adm.metal
  - core/src/feature/metal/metal_float_adm_math.h
invariant: float_adm options reach kernels (ADR-1220).
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# float_adm options must reach the kernels (ADR-1220)

`adm_p_norm` (`apn`), `adm_bypass_cm` (`bcm`) and `adm_skip_scale0`
(`ssz`) are `VMAF_OPT_FLAG_FEATURE_PARAM` options `float_adm_metal.mm`
declares. Until ADR-1220, kernels hardcoded cube sum. Host pooling
hardcoded `1.0f / 3.0f` root. `bcm` was read by nothing. `ssz` zeroed
only reported `adm_scale0` sub-score while still folding scale 0 into
pooled `adm2` / `aim`.

Invariants:

- `FadmCsf` in `float_adm.metal` and `FadmCsfHost` in
  `float_adm_metal.mm` must stay byte-identical. Two former padding
  slots now carry `p_norm` (float) and `bypass_cm` (uint), so size
  and alignment unchanged; new option goes into **both** structs in
  same commit.
- `adm_p_norm` has four application points (DLM sum, CSF sum, pooling
  root, `get_noise_constant`); keep CPU's `p == 3` literal-cube fast
  path so default path cannot move.
- `adm_bypass_cm` gates BOTH DLM and AIM `adm_cm()` call — `adm.c`
  passes it to each.
- `adm_skip_scale0` is POOLING rule: `num_scale = 0`,
  `den_scale = 1e-10` for scale 0. No kernel change needed, because
  `adm_dwt2_lo_s` writes only `band_a`, which `adm_dwt2` computes
  identically.

No Apple hardware in dev fleet, so these asserted by construction
against CPU reference and CUDA twin rather than measured. Treat any
Metal float-ADM change as unverified until someone runs
`test_metal_float_adm_parity` on real silicon.
