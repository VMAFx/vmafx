- **Build:** NVTX builds discover `nvtx3/nvToolsExt.h` from the CUDA toolkit
  (the nvcc root or conventional toolkit prefixes) instead of hardcoding
  `/usr/local/cuda/include` for both NVTX consumers (issue 3091).
