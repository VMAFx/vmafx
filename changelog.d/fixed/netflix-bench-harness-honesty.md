- **`testdata/bench_all.sh` no longer reports a hard GPU failure as an
  absent backend.** It sent the `vmaf` binary's stderr to `/dev/null` and
  printed `SKIP (vmaf exited N — backend likely unavailable)` for any
  non-zero exit, so the CUDA and SYCL rows masked a real abort: on the
  bench host both backends exit 234 with `context could not be
  synchronized` / `problem flushing context` whenever `--threads` is
  passed, which the script hard-codes as `--threads 1`. Stderr is now
  captured to `$OUTDIR/<row>.err`, the row prints `FAIL` with the exit
  code and the last stderr line, and the comparison block says `NO DATA`
  instead of guessing at a cause. The three backend flag sets also drop
  `--no_vulkan`, which current CLI builds reject as unrecognized since
  ADR-0726 removed the Vulkan backend. Filed as
  `T-GPU-CLI-THREADS-CTX-SYNC-2026-09-06` and
  `T-CUDA-FFMPEG-FILTER-NONDETERMINISM-2026-09-06` in `docs/state.md`.
- **`testdata/benchmark_netflix.py` no longer hard-codes host-specific
  paths.** The FFmpeg binary defaulted to `/home/kilian/dev/ffmpeg-8/ffmpeg`,
  which no longer exists, and the SYCL/QSV import path was pinned to
  `/dev/dri/renderD130`, which on the bench host is now the AMD iGPU — the
  run failed with `unsupported drm device by media driver: amdg`. The
  defaults are now the container's `/usr/local/bin/ffmpeg` and
  `/dev/dri/renderD128`, both overridable via `VMAF_FFMPEG` and the new
  `VMAF_SYCL_RENDER_NODE` (same pattern as ADR-0792).
