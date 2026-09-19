- **The three HIP integer ADM tests are no longer registered `should_fail`,
  and the two shared border/rounding tests now compare real scores.**
  `test_hip_adm_parity`, `test_hip_adm_small_border` and
  `test_hip_adm_wide_rounding` kept `should_fail : true` from the ADR-1154
  picture-staging deferral after ADR-1211 fixed it, so `meson test` on a HIP
  device failed on an unexpected pass. The two shared CUDA/HIP sources also
  asked for `VMAF_integer_feature_adm3_score`, which the HIP twin does not
  emit, so they failed before comparing anything; they now skip that name
  under `HAVE_HIP`, name the failing feature, and fill their pictures from a
  lowbias32 texture instead of a smooth ramp, on which the border defect
  ADR-1167 fixed is detectable (adm2 moves by 4.0e-4 against the 1e-4 gate;
  the ramp gave 7.5e-6). Clean HIP scores stay bit-identical to the CPU.
