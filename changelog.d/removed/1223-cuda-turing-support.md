- **Breaking (GPU hardware):** the CUDA backend now requires **compute
  capability 8.0 (Ampere) or newer**. Turing (`sm_75` — RTX 20xx,
  GTX 16xx, Tesla T4) and older are no longer supported: the `sm_75`
  cubin and the CUDA-12-only `compute_50` PTX are gone, and the
  `clang`-CUDA fallback path targets `sm_80`. CUDA 13.x had already
  dropped Maxwell, Pascal and Volta, so Turing was the last pre-Ampere
  architecture still receiving a cubin. Affected users can run the CPU
  backend (`-Denable_cuda=false`) or stay on v3.2.1. `vmaf_cuda_state_init()`
  now returns `-ENOTSUP` with an explicit message naming the device, its
  capability and the required floor, instead of letting `cuModuleLoadData`
  fail with `CUDA_ERROR_NO_BINARY_FOR_GPU` inside a feature extractor.
  See ADR-1223.
