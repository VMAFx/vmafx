---
paths:
  - core/src/feature/hip/integer_adm_hip.c
  - core/src/feature/hip/ciede_hip.c
invariant: Scaffolding posture returns -ENOSYS and performs no uninitialized operations.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Scaffold posture returns -ENOSYS, nothing else (ADR-1264)

`enable_hipcc` defaults **false** -> no device kernels -> extractor must report
`-ENOSYS`. Parity tests turn that into `[skip: ...]` and pass.

Scaffold path returns `-ENOSYS` **directly**. Never call kernel-submit helper
with placeholder args first: `vmaf_hip_kernel_submit_pre_launch(..., NULL, ...)`
rejects NULL `rb` as its first statement, so it always returns `-EINVAL` and any
`return -ENOSYS` after it is dead code. That shape failed
`test_hip_float_vif_parity` and `test_hip_psnr_hvs_parity*` on every default
build.

Writing new HIP parity test: check `-ENOSYS` at **both** sites --
`vmaf_use_feature()` AND `vmaf_read_pictures()`. Extractor may give up at
registration or inside `extract()`; `speed_temporal_hip` does latter.
Reference shape: `core/test/test_hip_speed_temporal_parity.c`.
