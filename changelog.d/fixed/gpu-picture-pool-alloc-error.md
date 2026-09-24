- Fixed `vmaf_gpu_picture_pool_init`: return `-ENOMEM` when allocation of the
  `VmafGpuPicturePool` struct fails, preserving the `*pool = NULL` contract.
  Previously returned 0 (`err` was initialized to 0), causing callers to treat
  failed pool allocation as success with a null handle (#1455).
