- Made the touched SYCL feature-kernel batch warning-free under oneAPI,
  clang-tidy, cppcheck, and the 60-line HISS policy without suppressions or
  numerical-contract changes. Intel AOT device arguments are now scoped to the
  native target while retaining the portable SPIR-V fallback.
