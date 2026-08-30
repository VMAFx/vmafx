- **C++23 Wave 8**: activate `opt.cpp` (completing Wave 1 / ADR-0721) and
  convert `read_json_model.c` → `read_json_model.cpp` (ADR-0761).
  Both compile under `cpp_std=c++23` in isolated static libraries linked into
  `libvmaf.so`. Public C ABI unchanged.
  `extern "C"` guards added to `opt.h`, `log.h`, `model.h`, `read_json_model.h`.
