<!-- markdownlint-disable MD013 -->
# Research 1185 — refreshing the per-backend performance baselines (2026-09-06)

Companion to [ADR-1185](../adr/1185-backend-perf-baseline-methodology.md).
Records what was measured, on what, and the two defects the measurement
exposed. Every figure below came from a command run on the `ryzen-4090-arc`
host on 2026-09-06; none is copied from a previous run or another document.

## 1. Why the old table could not simply be re-run

`docs/benchmarks.md`'s reproduce block said:

```bash
CC=icx CXX=icpx meson setup core/build libvmaf ...
```

`libvmaf/` stopped being the meson source root at ADR-0700. Running it
verbatim gives:

```text
ERROR: Neither source directory 'core/build' nor build directory None
contain a build file meson.build.
```

Corrected to `meson setup core/build core` in this PR. This is the whole
reason the doc-drift rule exists: the reproduce block had been wrong for as
long as the rename has been in, and nobody re-ran it.

Second obstacle: the fixture directories are gitignored
(`python/test/resource/yuv/` at `.gitignore:199`, `testdata/bbb/*.yuv` at
`:51`), so a fresh `git worktree` has neither. The failure mode is
`could not open file: python/test/resource/yuv/src01_hrc00_576x324.yuv`,
which reads like a build problem and is not.

## 2. What was measured

Harness: `testdata/bench_backends.py` (new). Median of 3 timed runs after one
discarded warmup, `--threads 1`, one exclusive `--backend` per run, load
average sampled around every cell. Fixtures: the three Netflix golden pairs
plus a 200-frame 4K BBB pair generated from
`.corpus/bbb_e2e/bbb_sunflower_2160p_30fps_normal.mp4` (`-ss 60` to skip the
title card, libx264 CRF 35 distortion). Models: `model/vmaf_v0.6.1.json` and
the resolved default `vmaf_v1.0.16_3d0h`.

Numbers are in [`docs/benchmarks.md`](../benchmarks.md#refreshed-per-backend-baselines-2026-09-06-ryzen-4090-arc).

**Measurement conditions were not clean.** The host ran a container rebuild
throughout. The 1-minute load average sat at 6.0–11.5 for most cells and
spiked to 52.6 during one 4K re-run, which produced a 102 % spread on that
cell. Cells are published with their load and spread so a reader can discount
them; the 4K rows are the ones to distrust most.

## 3. Defect 1 — no GPU backend completes a scored run on `master`

`origin/master` at `cd52f2670`. CUDA, SYCL and HIP all abort:

```text
libvmaf WARNING feature "VMAF_integer_feature_motion2_score" cannot be overwritten at index 42
libvmaf WARNING feature "VMAF_integer_feature_motion3_score" cannot be overwritten at index 42
...  (indices 42 through 46)
libvmaf ERROR context could not be synchronized
problem flushing context
```

It is length-dependent. `src01_hrc0{0,1}_576x324_5frames.yuv` passes on CUDA;
the 48-frame pair fails; the 200-frame 4K pair fails. The 3-frame checkerboard
pair passes on CUDA and fails on SYCL.

Mechanism, on the CUDA side:

1. `flush_context_cuda()` (`core/src/libvmaf.c:2160`) drains `gpu_pending` by
   calling `vmaf_feature_extractor_context_collect()`, which emits the tail
   batch's motion2/motion3 scores.
2. It then also calls `vmaf_feature_extractor_context_flush()` on the same
   extractor. The `continue` that would skip this is guarded by
   `vmaf->thread_pool &&  ... TEMPORAL`, so it does not fire on the
   single-threaded path.
3. `flush_fex_cuda()` re-emits the same tail range through
   `emit_batch_scores()` (`core/src/feature/cuda/integer_motion_cuda.c:591`),
   which appends with the **non-idempotent**
   `vmaf_feature_collector_append_with_dict()`.
4. `feature_vector_append()` (`core/src/feature/feature_collector.c:243`)
   refuses a second write to an already-`written` slot, logs the warning and
   returns `-EINVAL`.
5. That `-EINVAL` is OR-ed into the same accumulator as the
   `cuCtxSynchronize()` result, so a bookkeeping duplicate surfaces as a
   context-synchronisation failure.

The same file's *trailing* single-index emit already guards with
`append_if_unwritten()` — the batch-range emit was simply missed.
`flush_fex_sycl()` (`core/src/feature/sycl/integer_motion_sycl.cpp:880`) has
no guard at all.

**Why CI does not catch it.** The only lane that builds CUDA + SYCL + HIP
together is `build.yml`'s "Linux Intel LLVM" job, and it runs on a GPU-less
runner. Every GPU-hardware lane builds a single backend. The combination
"GPU present, clip longer than one motion batch" is not exercised anywhere.

**Partial fix, verified but deliberately not shipped here.** Routing
`emit_batch_scores()`'s three appends through the existing
`append_if_unwritten()` helper makes CUDA complete: the 48-frame pair scores
`76.682792` with `keys=14`, and the 4K pair reaches 167.16 fps (0.8 % spread
over 5 reps). SYCL still fails
afterwards at index 46 for a second cause that was not root-caused, and HIP
fails with no duplicate-write warning at all, so it is a third cause. A
one-of-three fix without the cross-backend parity gate does not belong in a
benchmarks PR; the defect is filed as
`T-GPU-MOTION-FLUSH-DOUBLE-EMIT-2026-09-06` in
[`docs/state.md`](../state.md) with the diff available for a dedicated PR.

Note that suppressing the duplicate cannot change any score: the second write
was already being *rejected*, so the stored value is the first one either way.
Only the error propagation differs.

## 4. Defect 2 — CUDA motion parity is 1.5e-2 off CPU on the 576×324 pair

With CUDA unblocked, `model/vmaf_v0.6.1.json` on the 48-frame src01 pair
pools `76.682792` (CUDA) against `76.667831` (CPU) — delta **1.50e-2**.
`docs/benchmarks.md` recorded a 6-decimal-place pool match for the same
fixture and model at commit `41301496`. The default model shows the same
shape (`82.823783` vs `82.816062`, delta 7.7e-3).

GPUs are not bit-exact to the CPU and never have been, but 1.5e-2 pooled is
far outside the places=4 bound the fork's own cross-backend gate uses, and it
is three orders of magnitude worse than the same table's historical entry.
Filed as `T-CUDA-MOTION-PARITY-576P-1.5E-2-2026-09-06`. Not bisected.

## 5. The default-model cost, which is what the retrain planning asked for

Ratios from the two tables, same fixture and backend within one session:

|Fixture|CPU default vs v0.6.1|CUDA default vs v0.6.1|
|---|---|---|
|src01 576×324, 48f|0.62×|0.31×|
|checkerboard 1-px 1080p, 3f|1.38×|0.59×|
|checkerboard 10-px 1080p, 3f|1.25×|0.58×|
|BBB 4K, 200f|1.22×|0.05×|

The CPU story is mild and resolution-dependent — the default model costs 38 %
more per frame at 576×324 but is *faster* at 1080p and 4K, so the v1 feature
set is a different mix rather than strictly more work.

The CUDA story is not mild. At 4K the default model runs at 9.00 fps against
the CPU's own 17.52 fps on the same model: **enabling the GPU makes the
default model twice as slow as not enabling it.** During that run `ps`
reported the process at 96.9 % CPU with `nvidia-smi` showing the RTX 4090 at
34 % utilisation — the signature of ADR-1183's twin gating dispatching
unsupported twins back to the host, so the run pays the transfer cost and then
does the work on the CPU anyway. `T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05`
is the same class of gap on the other two backends.

For retrain planning the actionable form is: **the default model has no
working GPU fast path today.** Any plan that assumes GPU throughput for v1
scoring needs the twin-coverage gaps closed first, and the flush defect in
§3 closed before any of it can even be measured.

## 6. What is still unknown

- SYCL's second flush failure cause (index 46 persists after the CUDA-shaped fix).
- HIP's flush failure cause (no duplicate-write warning at all).
- Whether the 1.5e-2 CUDA motion parity delta predates the flush defect.
- Clean-machine numbers. Every figure here carries container-rebuild load;
  a quiesced re-run would tighten the 4K rows in particular.
