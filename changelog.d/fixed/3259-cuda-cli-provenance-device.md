- **Name the CUDA device, its ordinal and driver version in JSON provenance on the CLI path (#3259).**
  When a `VmafCudaState` is imported into a `VmafContext` via `vmaf_cuda_import_state()` (such as
  `vmaf --backend cuda --json`), `vmafx_provenance_device()` now describes the imported state's
  `CUdevice` so `device_name`, `device_index` and `device_runtime` report the CUDA device name,
  ordinal and `"cuda driver X.Y"` instead of `"cuda"`, `-1` and `"unknown"`.
