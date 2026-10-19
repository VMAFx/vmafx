- **Unused code, ignored attributes and deprecated calls no longer print compiler warnings.**
  Test tables and helpers that only a skipped or disabled configuration reaches are compiled only
  there (`-Wunused-function` / `-Wunused-variable` in the ms_ssim_decimate, cambi, ssimulacra2 and
  read_pictures tests, the registry helpers of `model_loader.c` on Windows, the HIP error mappers
  without `hipcc`); `VMAF_EXPORT` is empty for GCC on MinGW, where the visibility attribute was
  ignored and drew a warning under LTO; the Win32 pthread shim spells `__stdcall` only in the host
  pass of a SYCL build; `VmafRef` is constructed with count 1 instead of calling the deprecated
  `std::atomic_init()`; the ONNX Runtime headers are `-isystem`; and a test that linked with an
  explicit `link_language : 'cpp'` no longer repeats `-lc++` on macOS. No score, symbol or
  option changes.
