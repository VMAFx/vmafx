---
paths:
  - .github/workflows/ffmpeg-integration.yml
  - .github/workflows/ffmpeg-patch-stack.yml
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
pristine tree sometimes emits generated-makefile status line. Release checkouts
use `scripts/ci/checkout-annotated-tag.sh`; direct shallow clones warn on
annotated FFmpeg tag. ordinary GCC/macOS matrix deliberately
avoids fork series to test stock FFmpeg surfaces against libvmaf, but
applies shared FFmpeg fix series alone (`scripts/ci/ffmpeg-shared-series.sh`,
ADR-3143) to harden pinned upstream source. SYCL lane applies shared series,
then replays complete fork series. Both paths require exact `git apply`; never
restore fuzz-capable `patch -p1` fallback or quiet compiler output.

`workflows/ffmpeg-patch-stack.yml` job `filter` = only gcc build of fork
series' own C code (vmafx filters, CLI glue). Same gate: configure
`--fatal-warnings`, tee make output to build log, fail on shared warning
pattern (issue 2477). Only grep exit 1 passes (`|| [[ $? -eq 1 ]]`, same form
as `ffmpeg-patches/test/build-and-run.sh`); exit 2 or unreadable log fails
step. `scripts/ci/test_ffmpeg_patch_workflow_contract.py` checks all parts and
runs gate on sample build logs.
