- Advanced the maintained FFmpeg baseline from `n9.0.1` to `n9.0.2`
  (`946fcce07b6dcd0331c8cc609192aeff5e1924f8`). The existing 18 integration
  patches remain byte-identical, and patch 0019 removes all 126 diagnostics
  exposed by GCC 14 production/FATE builds and independent GCC 16
  production/full-target builds,
  without suppressions or disabled codecs.
  All 19 patches replay cumulatively; every maintained FFmpeg image, hosted
  integration lane, and patch smoke build now fails on configure or compiler
  warnings. Hosted integration and smoke builds compile every test program and
  run generated FATE targets in that gate, and the previously broken
  `Dockerfile.ffmpeg` path uses the canonical series. Exact
  patch application replaces every fuzzy fallback. The dev image also fails if
  a promised encoder is absent. FFmpeg 9's removed `--enable-libnpp` no-op is
  omitted. Annotated AMF/FFmpeg tags are checked out through their peeled
  commits, eliminating Git's shallow-clone warnings without losing local
  version tags.
- Repaired the node and Go-server partial libvmaf builders uncovered by the
  integration run: configure inputs are complete, built-in models are embedded,
  LTO retains parallel workers, SONAME symlinks survive staging, and FFmpeg
  consumes Meson's interface-versioned `libvmaf.pc` instead of the product tag.
  The no-built-in-model configuration now compiles with warnings-as-errors and
  has a passing model test.
- Replaced warning-producing manual C/C++ standard flags with Meson's ordered
  built-in preferences while retaining the `std::expected` library probe.
