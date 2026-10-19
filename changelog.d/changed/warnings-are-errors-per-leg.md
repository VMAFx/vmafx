- **A compiler or linker warning now fails the CI leg that prints none today.** The gated legs of the
  build matrix (gcc, clang, Apple clang, icx / icpx, MinGW, CUDA and HIP builds), the ASan, UBSan and
  TSan builds, and the libvmaf builds of the Go, Rust and FFmpeg jobs pass `-Dwerror=true` and the
  linker's fatal-warnings switch through `scripts/ci/werror-args.sh`; with it `-Dwerror=true` also
  reaches the nvcc (`--Werror all-warnings`) and hipcc (`-Werror`) device compiles. Legs that are not
  at zero yet stay as they were and are listed with their cause in
  [the CI overview](docs/development/ci.md#warnings-are-errors-adr-2170). Release builds and container
  images do not use the switch. See [ADR-2170](docs/adr/2170-warnings-are-errors-per-leg.md).
