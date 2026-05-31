<!-- markdownlint-disable MD013 MD060 -->
# ADR-0383: K150K corpus scoring driver — parallel CPU worker redesign

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `ai`, `corpus`, `performance`, `training`, `fork-local`

## Context

The K150K-A corpus scoring driver (`ai/scripts/extract_k150k_features.py`, ADR-0362) ran
clips serially: one vmaf invocation at a time, using `core/build-cpu/tools/vmaf`.  At
7.1 s per 540p 5-second clip with 4 CPU threads, the serial baseline achieved 0.14 clip/s
— a 296-hour projected runtime for all 152,265 clips.

The original plan was to switch to a CUDA-enabled binary and use `--backend cuda` to
accelerate per-clip scoring.  Investigation revealed two blockers:

1. **CUDA slower than CPU for 540p 5 s clips.**  Benchmarking the CUDA binary against the
   same clips showed 24–26 s/clip with `--threads 1 --backend cuda`, versus 7.1 s/clip on
   CPU with `--threads 4`.  GPU CUDA-context-init overhead dominates for short clips at
   sub-HD resolution; the compute kernels themselves are not the bottleneck.

2. **CUDA binary double-write bug (regression from commit `30179695a`, April 28).**  The
   `feature_extractor_list[]` table in `core/src/feature/feature_extractor.c` had 6 CUDA
   extractors registered twice (psnr_cuda, float_moment_cuda, ciede_cuda, float_ssim_cuda,
   float_ms_ssim_cuda, psnr_hvs_cuda), causing "cannot be overwritten" warnings for those
   features.  After the dedup fix, a deeper issue remained: when no explicit `--model` is
   provided, the CLI auto-loads `vmaf_v0.6.1` as the default model, which registers CUDA
   twins (adm_cuda, vif_cuda, motion_cuda) via `vmaf_use_features_from_model()`.  The
   subsequent `--feature adm` call registers the CPU "adm" extractor; since the dedup in
   `feature_extractor_vector_append()` compares extractor names ("adm" vs "adm_cuda"), it
   does not catch this as a duplicate.  Both extractors run and write the same collector
   slots — producing "cannot be overwritten" warnings for adm, vif, and motion at every
   frame.

Given these findings, the 5× throughput target is achievable purely through
**process-level parallelism on the CPU binary**: 8 parallel workers × 7.1 s/clip yields
~0.89 clip/s theoretical (0.5–0.7 clip/s with I/O and orchestration overhead) — a 4–5×
speedup over the 0.14 clip/s serial baseline.

## Decision

We will redesign `ai/scripts/extract_k150k_features.py` to use
`concurrent.futures.ProcessPoolExecutor` with a configurable number of workers
(`--threads-cuda`, default 8), each independently decoding one clip to a private YUV
scratch file, scoring it via `core/build-cpu/tools/vmaf`, aggregating frame metrics,
and cleaning up the scratch file.  The main process collects results, writes the `.done`
checkpoint, and flushes the parquet periodically.

The default binary remains `core/build-cpu/tools/vmaf`.  The `--threads-cuda` flag
retains its name for CLI compatibility; the workers run on CPU regardless of backend.
The `--no-cuda` flag passes `--no_cuda --no_sycl --no_vulkan` to the vmaf binary for
explicit CPU-only operation.

The `.done` checkpoint file and existing partial progress (5,628 clips already scored)
are preserved — the redesign is a drop-in replacement that resumes from where it left off.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| CUDA binary + `--backend cuda` | GPU acceleration per clip; natural fit for the RTX 4090 | 24–26 s/clip (3.5× slower than CPU) for 540p 5 s clips; double-write bug in current master that requires a non-trivial fix to the vmaf CLI (model auto-load vs explicit `--feature` interaction) | GPU is not the bottleneck for this clip size; bug is traceable to a CLI design issue in the model-auto-load path that is out of scope for the corpus-scoring sprint |
| CUDA binary + `--no-prediction` | Avoids default model loading; prevents the adm_cuda double-write | `--no-prediction` is not implemented in the current fork build; would require another C change | Out-of-scope C change for a corpus-driver task |
| Serial CPU (status quo) | Simple; no parallelism complexity | 0.14 clip/s; 296 h for the full corpus | Does not meet the 5× throughput target |
| Threads-based parallelism (multithreading within one process) | Lower memory overhead than multiprocessing | libvmaf vmaf C binaries are not thread-safe for concurrent scoring pipelines; ProcessPoolExecutor provides full isolation | Process isolation is required |

## Consequences

- **Positive**: ~0.5–0.7 clip/s at 8 workers (4–5× speedup). Checkpoint is preserved and
  resumes correctly. Per-worker YUV isolation eliminates scratch-file collisions. The
  parquet flush is atomic (`.tmp` rename). Worker failures are isolated — one bad clip
  does not abort the run.
- **Negative**: 8 parallel `vmaf` processes consume ~32 CPU threads total (4 threads/worker)
  and ~8 × 120 MB = ~960 MB of peak YUV scratch space. On machines with fewer cores,
  `--threads-cuda` should be reduced.
- **Neutral / follow-ups**: The CUDA double-write bug in the CLI model-auto-load path
  remains open; it should be fixed before the next attempt to use `--backend cuda` in any
  batch pipeline.  A follow-up investigation is tracked in
  `docs/state.md` §Open.  The duplicate extractor registration bug in
  `feature_extractor_list[]` (6 extractors registered twice, introduced by commit
  `30179695a`) has been fixed in this PR.

## References

- ADR-0346: FR-from-NR adapter pattern.
- ADR-0362: K150K corpus integration design.
- Research-0096: K150K GPU driver investigation — CUDA timing, double-write root cause.
- PR: `#<tbd>` (this change).
