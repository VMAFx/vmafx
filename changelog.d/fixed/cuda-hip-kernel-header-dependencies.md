- Track shared header dependencies for CUDA fatbin and HIP HSACO device targets
  using complete repo-local include-closure `depend_files`, the generated CUDA
  config header, and compiler depfiles, ensuring header-only changes trigger
  incremental Ninja rebuilds across all 44 device targets. The device-free
  contract is backend-aware and remains green in CPU-only build directories.
