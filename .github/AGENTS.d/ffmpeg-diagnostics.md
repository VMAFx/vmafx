---
paths:
  - .github/workflows/ffmpeg-integration.yml
  - .github/workflows/ffmpeg-patch-stack.yml
  - ffmpeg-patches/series.txt
  - Dockerfile
  - Dockerfile.ffmpeg
  - dev/Containerfile
  - docker/Dockerfile.node
invariant: FFmpeg builds configure with --fatal-warnings; every warning gate fails closed on a broken scan; exact git apply.
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

Every build-log warning gate fails closed on its own scan failing, not only on
a matched warning: only grep exit 1 (no match) reads as clean (issue 3361).
The two `ffmpeg-integration.yml` gates, the `Dockerfile.ffmpeg` gate and the
`ffmpeg-patch-stack.yml` filter gate take `elif [ $? -gt 1 ]` / the
`|| [[ $? -eq 1 ]]` guard; the three patched-file-filtered builders (root
`Dockerfile`, `dev/Containerfile`, `docker/Dockerfile.node`) write the scan to
`/tmp/ffmpeg-warnings.txt` behind `|| [ $? -eq 1 ]` and filter that file, so a
missing build log aborts the build instead of feeding the filter an empty
match list. The filter takes its own `elif [ $? -gt 1 ]` arm, so a missing
patched-file list aborts the build instead of reading as no match.
`test_ffmpeg_patch_workflow_contract.py` holds the guarded forms statically and
replays the unfiltered gates plus the root Dockerfile's filtered gate on sample
logs.
