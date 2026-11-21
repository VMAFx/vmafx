- **Build:** NVTX builds take the toolkit include directory from Meson's
  `dependency('cuda')`, which reads `CUDA_PATH` / `CUDA_HOME` / `CUDA_ROOT`
  and the conventional toolkit roots, instead of hardcoding
  `/usr/local/cuda/include` for both NVTX consumers. The Ubuntu CUDA
  build-matrix leg now configures `-Denable_nvtx=true` (issue 3091).
