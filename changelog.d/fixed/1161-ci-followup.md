- Restored the remaining cross-platform and quality gates after the C23/C++23
  toolchain migration: Windows path creation now accepts both separator styles,
  MSVC receives only compiler-supported warning/visibility flags, Windows oneAPI
  builds use the `c++latest` mode required by `std::expected`, and Linux SYCL
  keeps GCC as the host compiler while compiling device objects with `icpx`.
- Updated the Python MCP server for the MCP SDK 2.x low-level handler API and
  Pydantic field naming. The complete MCP suite now passes against `mcp==2.1.1`.
- Added direct regression coverage for ONNX Runtime double, int64, int32, and
  unsupported output conversions, keeping `ort_backend.c` above its ratcheted
  coverage floor without weakening the threshold.
- Fixed unsigned output formatting, checked luminance-range return values, and
  aligned scalar float-ADM contraction with its explicit non-FMA NEON twin.
- Brought the dev-MCP image back in sync with the FFmpeg n9.0.1 patch base and
  copied the complete Go module inputs into its six-binary build stage.
- Preserved C linkage for the shared minunit test counter so C++ tests link on
  MSVC as well as GCC and Clang.
