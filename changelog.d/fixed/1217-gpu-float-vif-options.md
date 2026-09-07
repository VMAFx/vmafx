- `vif_sigma_nsq` and `vif_enhn_gain_limit` now reach the CUDA, SYCL and
  HIP `float_vif` compute kernels. All three hardcoded the two defaults
  as local constants, so a non-default value was accepted,
  range-checked, folded into the derived feature name, and then
  silently ignored. This was reachable from a shipped model:
  `model/vmaf_float_v0.6.1neg.json` sets `vif_enhn_gain_limit = 1.0` on
  all four VIF scales — the setting that makes it the NEG model — so a
  GPU run of the NEG model published ordinary enhancement-gain-enabled
  scores under the NEG feature keys. Measured drift at
  `egl=1.0 snsq=1.5`: `1.69e-02` (CUDA), `8.37e-04` (SYCL) and `5.09e-03` (HIP)
  against a `1e-4` gate. The three `test_<backend>_float_vif_parity` tests now
  each pin the NEG option set; `float_vif_hip` gained its first parity
  test. Re-score any NEG-model run made on a GPU backend. See ADR-1217.
