- **Editing a header now rebuilds the CUDA and HIP kernels that include it.**
  The nvcc and hipcc build steps did not record header dependencies, so an
  incremental build after a header change could link host code against
  kernels compiled for an older struct layout, which crashed or, worse,
  computed wrong results. Clean builds were never affected.
