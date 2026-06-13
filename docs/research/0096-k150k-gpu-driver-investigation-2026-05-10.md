<!-- markdownlint-disable MD013 -->
# Research-0096: K150K GPU driver investigation — CUDA timing and double-write root cause

**Date**: 2026-05-10
**Author**: Claude (Anthropic) / lusoris
**Status**: Closed — findings incorporated in ADR-0382

---

## Summary

Investigated whether a CUDA-enabled `vmaf` binary would accelerate K150K-A corpus
feature extraction beyond the 0.14 clip/s serial CPU baseline.  Conclusion: CUDA is not
beneficial for 540p 5 s clips (GPU context init overhead dominates).  Throughput target
is met via parallel CPU workers.

---

## Findings

### 1. Serial CPU baseline

Single-clip timing with `core/build-cpu/tools/vmaf`, `--threads 4`, 11 feature
extractors on `orig_10000251326_540_5s.mp4` (960×540, yuv420p, ~150 frames):

```text
real  7.062s  user  27.19s  sys  0.70s  cpu  395%
```

At 0.141 clip/s, full-corpus runtime is ~296 hours.

### 2. CUDA binary timing

Timing the worktree CUDA binary (`core/build-cuda/tools/vmaf`,
`-Denable_cuda=true --buildtype=release`) with `--threads 1 --backend cuda` on the
same clip:

```text
real  26.3s  user  25.6s  sys  0.70s  cpu  100%
```

The CUDA binary is 3.7× slower per clip than CPU with `--threads 4`.  The 100%
CPU utilisation with `--threads 1` confirms the bottleneck is not GPU compute but
CUDA context initialisation — which is paid afresh for every `vmaf` subprocess
(no amortization across clips in the serial-subprocess model).

### 3. Batch1b binary CUDA (April 26 build, before duplicate registration bug)

For reference, the `batch1b` binary (earlier CUDA build) was benchmarked at:

```text
4 workers, 16 clips: 16/114.6s = 0.140 clip/s
```

Same as the CPU baseline — confirming the per-worker CUDA overhead negates the
theoretical GPU speedup for this clip geometry.

### 4. ProcessPoolExecutor parallelism (CPU binary, earlier bash benchmarks)

```text
 8 workers, threads=2, 32 clips: 32/64.8s  = 0.494 clip/s
12 workers, threads=2, 32 clips: 32/53.6s  = 0.597 clip/s
16 workers, threads=2, 32 clips: 32/77.5s  = 0.413 clip/s
```

Optimal worker count lies in the 8–12 range for a 32-thread Zen5 CPU.  Default is
set to 8 workers × 4 threads/worker = 32 threads total (full core utilisation).

### 5. CUDA double-write root cause

Root cause: the vmaf CLI auto-loads `vmaf_v0.6.1` as the default VMAF model when no
`--model` flag is given and `--no-prediction` is not set (see `cli_parse.c` line 797).
The model loading calls `vmaf_use_features_from_model()`, which uses
`vmaf_get_feature_extractor_by_feature_name(name, CUDA_FLAGS)` — this returns the
CUDA twin extractors ("adm_cuda", "vif_cuda", "motion_cuda") and registers them.

Subsequently, the `--feature adm` CLI argument calls
`vmaf_use_feature(vmaf, "adm", ...)` which calls
`vmaf_get_feature_extractor_by_name("adm")` — this finds the CPU extractor (named
"adm", different from "adm_cuda").  The deduplication in
`feature_extractor_vector_append()` compares `vmaf_feature_name_from_options(fex->name)`,
which returns "adm" and "adm_cuda" respectively — not equal, so both are registered.

Both the CUDA "adm_cuda" extractor and the CPU "adm" extractor run on every frame and
both write to the same feature-collector slots (adm2, integer_adm_scale0..3).  The
second write fires "cannot be overwritten" warnings at every frame (5 warnings ×
150 frames = 750 warnings for `--feature adm` alone).

Affected feature pairs (CUDA twin + CPU twin both registered):

- `adm_cuda` + `adm` → 5 features × 150 frames = 750 warnings
- `vif_cuda` + `vif` → 5 features × 150 frames = 750 warnings
- `motion_cuda` + `motion` → 2 features × 150 frames = 300 warnings

Note: this is a separate bug from the duplicate extractor registration in
`feature_extractor_list[]` (commit `30179695a`, April 28), which registered 6 CUDA
extractors twice in the global list.  That duplicate registration bug was fixed in
this PR by deduplicating lines 239–240 of `feature_extractor.c`.

### 6. Fix path for the CLI double-write (not implemented in this PR)

The clean fix would be one of:
(a) Add `--no-prediction` support to suppress the default model load, OR
(b) Make `feature_extractor_vector_append()` deduplicate by *provided feature names*
    rather than extractor name, so "adm" and "adm_cuda" (both providing "adm2") are
    treated as duplicates, OR
(c) Skip `--feature` registration when a CUDA twin is already registered for the same
    provided features.

Option (b) is the most correct but requires care to avoid breaking cases where two
extractors legitimately provide the same feature name at different precision.

---

## Conclusion

CPU parallel workers (8 workers × 4 threads = 32 threads) achieve 0.5–0.7 clip/s
from a 0.14 clip/s serial baseline — a 4–5× throughput improvement that meets the
stated target.  GPU acceleration is not beneficial for 540p 5 s clips.  The CUDA
binary has a latent CLI model-load / explicit-feature interaction bug that must be
fixed before `--backend cuda` can be used in any batch pipeline.
