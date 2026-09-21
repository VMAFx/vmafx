- **CUDA integer ADM kernels compile without NVCC diagnostic `#68-D`.** The
  historical Netflix-compatible negative rounding term is now spelled directly
  as `INT32_MIN` instead of being produced by an out-of-range unsigned-to-signed
  conversion. CUDA 13.4 emits no warning, touched kernels satisfy the whole-tree
  standards contract, and their runtime output remains exactly identical to the
  base build across the existing CUDA ADM regression executables.
