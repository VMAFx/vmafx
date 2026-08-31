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
  stabilized ARM float-ADM DWT2 on +0-initialized split multiply/add arithmetic
  by applying contraction-off only to the scalar DWT2 function and its NEON
  twin, including scalar-identical signed-zero behavior.
- Preserved the immutable Darwin AArch64 integer-ADM score through a named
  production compatibility wrapper that applies the historical three-tap rule
  only to the first DWT2 output column. The universal NEON kernel remains
  four-tap and scalar-bit-exact on Linux ARM and in direct parity tests.
- Brought the dev-MCP image back in sync with the FFmpeg n9.0.1 patch base and
  copied the complete Go module inputs into its six-binary build stage.
- Preserved C linkage for the shared minunit test counter so C++ tests link on
  MSVC as well as GCC and Clang.
- Pinned changed-file clang-tidy jobs to LLVM 22 instead of an ambiguous
  system alternatives link that could keep resolving to LLVM 18 and fail to
  parse C++26 `std::expected`.
- Removed the stale, unbuilt C++ model-test twin; Meson has always registered
  the actively maintained C test, while the unused copy had no compile-database
  entry and drifted behind later regression coverage.
- Made the C++ dictionary merge test check its final setup insertion instead of
  discarding the return code, satisfying both the test contract and Clang-Tidy.
- Replaced the overly broad scalar ADM contraction pragma with a function-scoped
  DWT2 guard. This preserves unrelated ADM arithmetic while keeping the DWT2
  kernel aligned with the immutable ARM golden-score contract.
- Let dev-container smoke tests inherit the image's CUDA, oneAPI, and ROCm
  runtime search paths instead of hiding `libirc.so` behind a host-style
  `/usr/local/lib` override; initialize the image paths without undefined
  Dockerfile variables.
- Identified the all-backend Linux lane as Intel LLVM and stopped running
  GCC-authored Python numeric snapshots there. Dedicated CPU/GCC jobs retain
  the complete tox and immutable Netflix golden gates; the Intel lane still
  runs the native Meson suite and every backend build.
- Made `make lint` regenerate Meson's compilation database through Ninja before
  invoking C analyzers, including on Ninja builds whose vendor-suffixed version
  string prevents Meson from generating that file during setup; missing C lint
  tools now fail the gate instead of claiming to skip successfully.
- Installed Xcode's separately shipped Metal compiler component in the combined
  macOS CPU + Metal build before Meson invokes `xcrun metal`.
