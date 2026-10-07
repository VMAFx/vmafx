---
paths:
  - core/src/feature/sycl/integer_adm_sycl.cpp
  - core/test/test_sycl_adm_parity.c
invariant: Integer ADM AIM pass; adm_sycl provides aim_score; 2 kernels per scale decouple CSF; adm_skip_aim mirrors CPU.
---
<!-- markdownlint-disable MD013 MD060 -->
# Integer ADM AIM pass (ADR-1362, T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05)

- `adm_sycl` provides `VMAF_integer_feature_aim_score` +
  `VMAF_integer_feature_adm3_score` -> default model `vmaf_v1.0.16_3d0h` runs
  its whole ADM branch on device. AIM = CPU `measure_aim`: threshold from
  csf(r) (3x3 `|csf(r)| / 30` + 1/15 centre), measure t - r, no noise floor.
- Per scale 2 kernels after DWT: `launch_decouple_csf` stores
  `d_csf_f` (`|csf(t - r)| / 30`) + `d_csf_f_aim` (`|csf(r)| / 30`) over
  whole band; `launch_csf_den_cm` = one work-group per region row, all 3
  bands, 9 sums (CM, CSF den, AIM). r and t - r recomputed per sample
  (`adm_dev_sample`); storing r measured slower on UHD 770 (bandwidth).
- One accumulator buffer `[term][scale][band]` (`adm_accum_slot`, 36 int64):
  one memset in `pre_fn`, one D2H in `post_fn`, read in `collect()` after
  `vmaf_sycl_graph_wait()`. No host wait inside frame; keep it that way.
- Row fold through `adm_cm_round_row_total()` in `adm_dev_fold_row`, once per
  row per sum (ADR-1167). `test_adm_cm_row_rounding_contract.py` pins it.
- `adm_dev_decouple_k` clamps Q15 quotient in int64 like CPU's
  `tmp_k`. Old kernel narrowed to int32 first -> |t / o| > 2^16 at scales 1-3
  wrapped -> `integer_adm_scale2` up to 1.40e-6 off CPU at 4K, aim not
  exact. Never
  narrow before clamp.
- All outputs (adm2, scale*, debug num / den, aim, adm3) finalised in
  CPU's own float arithmetic (`adm_cm_scale_cpu`, `adm_den_scale_cpu`,
  `adm_scale_cpu`, `adm_terms`, `adm_finalise`: float per band, float per
  scale, double sum, `(float)1e-10` skip-scale0 den, floor in place) ->
  bit-exact. Maintainer contract: bit-exact with CPU. Any double
  shortcut here breaks `test_sycl_adm_parity` / `test_sycl_adm_tiny_frames`.
- CM kernel sub-group size 16: 9 int64 sums spill at 32 lanes on Xe-LP.
- `adm_skip_aim` mirrors CPU: AIM sums skipped, aim = 0.
- Non-integer `adm_enhn_gain_limit` (e.g. 1.2): bit-exact too (ADR-1413).
  `adm_dev_gain_limit()` -> `adm_gain_limit_product()` = `trunc(fl(r * gain))`
  from integers. Do not bring back Q31 (`gain_limit_to_q31`): floor + limit
  rounded to 2^-31 -> `5 * 1.2` = 5, CPU stores 6; scale0 up to 3.6e-5 off.
  Guard: `test_gpu_adm_fractional_gain_limit_parity`, `test_adm_gain_limit`.
- Guards: `test_sycl_adm_parity` (`test_adm_cpu_sycl_aim_bit_exact`),
  `test_sycl_adm_tiny_frames` (aim / adm3 bit-exact under `HAVE_SYCL`: tiny,
  noise, 16-bit, CSF modes 0-3), `python/test/gpu_default_model_test.py`.
