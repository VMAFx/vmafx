- **Backend migration table for the VMAFx C API (issue 2436).** The API
  reference now maps each task a program does with the libvmaf CUDA, SYCL,
  HIP or Metal backend functions (find, open and close a device, preallocate
  and fill frames, hand over decoder output, know when a frame is free,
  profile kernels) to its one VMAFx successor
  ([Backends that still use the engine's functions](docs/api/vmafx/index.md)).
