- **build:** Restore `integer_ssim_moments_t` typedef for non-x86 platforms. The type was
  defined only in `core/src/feature/x86/integer_ssim_avx2.h` (included under `#if ARCH_X86`),
  but used unconditionally in `integer_ssim.c` for scalar wrapper functions and function
  pointer typedefs. macOS arm64 / Windows arm64 builds failed with eight "unknown type name"
  errors. Fix: promote the typedef to a new shared `core/src/feature/integer_ssim.h` header
  included unconditionally; update the x86 header to pull from the shared header. (ADR-1040)
