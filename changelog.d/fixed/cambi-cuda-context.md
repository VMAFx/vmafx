- **CUDA CAMBI extractor failed with `CUDA_ERROR_INVALID_CONTEXT`.**
  During execution of the default model `vmaf_v1.0.16_3d0h` under
  `--backend cuda`, the CAMBI feature extractor crashed during
  synchronous `cuMemcpyDtoH` readback in `submit_fex_cuda` because the
  calling thread had no CUDA context current. Fixed by pushing
  `fex->cu_state->ctx` at submit entry and popping it cleanly on all exit
  paths, as well as aligning CAMBI options and TVI initialization with the
  CPU extractor (`integer_cambi_cuda.c`).
