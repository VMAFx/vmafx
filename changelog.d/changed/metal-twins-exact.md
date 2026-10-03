- **Metal kernels compile without fast math or FP contraction**
  (ADR-1498): every `.metal` file takes `-fno-fast-math -ffp-contract=off`,
  so fp32 `+ - * /`, `sqrt` and `fma` are correctly rounded and no `a * b + c`
  is fused behind the source's back, the policy the CUDA, HIP and SYCL
  kernels already follow. The arithmetic of a ported Metal twin lives in a
  header on `core/src/feature/metal/metal_portable.h` that also compiles on
  the host, where a test holds it against the CPU extractor. Guide:
  `docs/backends/metal/index.md`.
