- **libvmaf symbol visibility** — `libvmaf.so.3` no longer exports 207 internal
  symbols (libsvm C API `svm_predict`/`svm_train`/…, pdjson `json_open_buffer`/…,
  SIMD kernel functions, internal helpers `aligned_malloc`/`aligned_free`/…).
  Added `-fvisibility=hidden` to the build and introduced `VMAF_EXPORT`
  (`__attribute__((visibility("default")))` on GCC/Clang,
  `__declspec(dllexport)` on MSVC) annotated on all 44 public `vmaf_*`
  declarations. Eliminates silent symbol interposition for downstream binaries
  that link both libvmaf and libsvm. New header `libvmaf/macros.h` installed
  alongside the existing public headers. See [ADR-0379](docs/adr/0379-libvmaf-symbol-visibility.md)
  and [Research-0092](docs/research/0092-round4-symbol-visibility-audit.md).
