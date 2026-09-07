- **`adm_p_norm` reaches the Metal `integer_adm` kernel.** The twin declared the
  option with the CPU's name, alias, default and range and flagged it
  `VMAF_OPT_FLAG_FEATURE_PARAM`, so the engine accepted it, range-checked it and
  folded it into the [ADR-1183](docs/adr/1183-model-options-gate-gpu-twin-selection.md)
  derived feature name — and then `conclude_adm_cm()` computed with a hardcoded
  `1.0f / 3.0f`. A run with `apn=2` published a p = 3 number under the key
  `integer_adm2_apn_2`. Metal was the only `integer_adm` twin affected: CUDA,
  SYCL and HIP all pass `adm_p_norm` into their reduction. Only the numerator is
  parameterised, matching the CPU, whose denominator
  (`adm_den_scale_finalise`) is a fixed cube root in every backend. The default
  path is bit-identical — `1.0f / (float)3.0` and `1.0f / 3.0f` are the same
  float — so no shipped score moves. `test_metal_integer_adm_parity` gains a
  `test_integer_adm_p_norm_reaches_kernel` variant; the pre-existing
  default-only test could not see this, because at p = 3 the hardcoded constant
  is the right answer.
