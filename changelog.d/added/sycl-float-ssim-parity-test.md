- **test(sycl)**: Add `test_sycl_float_ssim_parity` — covers
  `vmaf_fex_float_ssim_sycl` registration and CPU-vs-SYCL parity at
  places=3 tolerance (Research-0985: accounts for the CPU/GPU formula
  difference and Arc A380 fp32 accumulation drift; fp64-capable hardware
  passes at places=4). Closes the coverage gap where `test_sycl_ssim_parity`
  only tested `integer_ssim_sycl`.
