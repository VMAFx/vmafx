- **ADM and SSIMULACRA 2 host conclusions are shared between the CUDA and HIP twins.** The
  SSIMULACRA 2 pooling weights and recursive Gaussian coefficients, the integer ADM per-scale
  results and the float ADM per-scale pooling now have one implementation each, in shared headers
  used by the CPU extractor and both twins. Internal refactor: scores are unchanged.
