---
paths:
  - .github/workflows/ffmpeg-integration.yml
  - ffmpeg-patches/series.txt
invariant: FFmpeg builds configure with --fatal-warnings and fail on diagnostics; exact git apply without fuzzy patch.
---
# FFmpeg diagnostics are fail-closed

Every FFmpeg build in `workflows/ffmpeg-integration.yml` configures with
`--fatal-warnings`, captures complete compiler log, and fails on GCC,
Clang, or NVCC warning diagnostics. build compiles all test programs and
runs every generated, sample-independent FATE target under same log gate,
so test translation units share warning-clean contract with production
objects. Capture `make -s fate-list` first and select only `fate-*` lines:
pristine tree may also emit generated-makefile status line. Release checkouts
must use `scripts/ci/checkout-annotated-tag.sh`; direct shallow clones warn on
annotated FFmpeg tag. ordinary GCC/macOS matrix deliberately
avoids fork series so it tests stock FFmpeg surfaces against libvmaf, but
applies shared FFmpeg fix series alone (`scripts/ci/ffmpeg-shared-series.sh`,
ADR-3143) to harden pinned upstream source. SYCL lane applies shared series,
then replays complete fork series. Both paths require exact `git apply`; never
restore fuzz-capable `patch -p1` fallback or quiet compiler output.
