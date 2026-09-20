- **Four HIP parity tests failed on an ordinary `-Denable_hip=true` build instead of
  skipping.** `enable_hipcc` defaults to false, so that build has no device kernels and
  the extractors are meant to report `-ENOSYS`, which every parity test turns into a skip.
  `float_vif_hip.c` and `integer_psnr_hvs_hip.c` instead returned `-EINVAL`: their scaffold
  path called `vmaf_hip_kernel_submit_pre_launch()` with a NULL readback buffer, and
  rejecting NULL is that helper's first statement, so the `return -ENOSYS` after it was
  unreachable. Separately `test_hip_speed_singular_parity.c` looked for `-ENOSYS` only at
  `vmaf_use_feature()`, while its extractor reports it from the frame submit. The default
  HIP fast suite goes from 173 ok / 4 fail to **177 ok / 0 fail**, and with
  `enable_hipcc=true` the same four run against real kernels and pass (183 ok / 0 fail).
  See [ADR-1264](docs/adr/1264-hip-scaffold-enosys-contract.md).
