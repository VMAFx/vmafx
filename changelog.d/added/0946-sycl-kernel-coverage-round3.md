## Added

- **SYCL kernel coverage round 3** (ADR-0946,
  `core/test/test_sycl_float_psnr_parity.c`,
  `core/test/test_sycl_float_adm_parity.c`,
  `core/test/test_sycl_float_vif_parity.c`,
  `core/test/test_sycl_float_motion_parity.c`,
  `core/test/test_sycl_psnr_hvs_parity.c`): five new CPU vs. SYCL
  parity gates at ADR-0214 places=4 (1e-4) tolerance for the
  float-VMAF family (`float_psnr_sycl`, `float_adm_sycl`,
  `float_vif_sycl`, `float_motion_sycl`) and the PSNR-HVS kernel
  (`psnr_hvs_sycl`). Lifts SYCL parity coverage from 50 % (after
  rounds 1+2) to 78 % (14 of 18 SYCL extractors). Each test mirrors
  the round-2 scaffold: 256x144 synthetic YUV420P fixture, places=4
  assertion, skip-on-no-device path. Round 4 backlog tracks the
  remaining 4 extractors (`float_moment_sycl`, `speed_chroma_sycl`,
  `speed_temporal_sycl`, `ssimulacra2_sycl`) — each needs scaffold
  extensions before the same gate can be added cleanly.
