<!-- ADR-1315 -->
- Drive public C API Doxygen warnings to zero and fail closed:
  remediated 228 warnings across `core/include/libvmaf/` by removing deprecated
  `@field` blocks, standardizing `@thread-safety` annotations to `@note Thread safety:`,
  splitting multi-variable member declarations, documenting `VmafPicture2` and
  nested score/config structs, and excluding the vendored Pelorus interop mirror.
  Configured `WARN_AS_ERROR = YES` in `core/doc/Doxyfile.public-api`, set
  `DOXYGEN_WARNING_CEILING: "0"` in `.github/workflows/doxygen-public-api.yml`,
  and added executable regression assertions in `core/test/test_gpu_public_header_docs.py`.
