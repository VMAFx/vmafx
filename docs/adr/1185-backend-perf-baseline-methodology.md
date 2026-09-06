<!-- markdownlint-disable MD013 -->
# ADR-1185: Per-backend performance baselines are median-of-N, one backend per build dir

- **Status**: Accepted
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: perf, benchmarks, cuda, sycl, hip, docs

## Context

The fork's per-backend throughput table in [`docs/benchmarks.md`](../benchmarks.md)
had drifted: its rows were captured on commit `41301496` against
`model/vmaf_v0.6.1.json`, before `vmaf_v1.0.16_3d0h` became the default model
(ADR-1169) and before the GPU twins of that model were made to run at all
(PRs #1307, #1312, #1324). Nothing in the table said what a caller who passes
no `--model` actually pays, which is precisely the figure retrain planning
needs.

Refreshing it exposed three methodology problems that the previous numbers
quietly had. First, `testdata/bench_all.sh` times **one** run per cell, so
every historical figure carries an unknown amount of scheduler noise; the
existing table reports a standard deviation over 5 reps for some rows but the
committed harness cannot produce one. Second, the reproduce block in
`docs/benchmarks.md` still said `meson setup core/build libvmaf`, a path that
stopped existing at ADR-0700 — so the documented procedure did not run. Third,
the fork's canonical bench host is a *daily-driver workstation*, not a
quiesced lab machine; a figure quoted without the load it was taken under is
not reproducible by anyone, including its author.

## Decision

We will measure per-backend baselines with a dedicated harness,
`testdata/bench_backends.py`, that reports the **median of at least 3 timed
runs after one discarded warmup**, alongside the min/max spread and the
1-minute load average sampled around every cell; it will engage exactly one
backend per run through the exclusive `--backend` selector, record the
`frames[0].metrics` key count as a backend-engagement check, and run every
fixture against both `model/vmaf_v0.6.1.json` (for continuity with the
historical rows) and the resolved default model (for the figure callers
actually pay). Each backend combination is benchmarked from **its own meson
build directory**, never from one all-backends binary.

## Alternatives considered

|Option|Pros|Cons|Why not chosen|
|---|---|---|---|
|**Chosen**: new `bench_backends.py`, median-of-N, per-backend build dirs|statistics are honest; `bench_all.sh` keeps its contract; the MCP `run_benchmark` wrapper (ADR-0517) is untouched|a second harness to maintain alongside `bench_all.sh`|—|
|Extend `bench_all.sh` in place with repetitions|one harness instead of two|`bench_all.sh` is consumed by the MCP `run_benchmark` tool and by `make bench`; its stdout shape is an interface. Adding repetition changes both its runtime (×4) and its output format|rejected: breaks a consumed interface for a benefit a sibling script delivers for free|
|Report mean ± standard deviation instead of median + spread|matches the existing table's notation|on a machine with a background container build, one stalled run drags the mean while the median is unmoved; sd of 3 samples is not meaningful anyway|rejected: the estimator has to survive the host we actually run on|
|Benchmark one all-backends binary (`cuda+sycl+hip`)|a single build, all rows comparable by construction|CI builds that combination only on GPU-less runners (`build.yml` "Linux Intel LLVM"), so its *runtime* behaviour is never exercised; benchmarking it would attribute a build-combination effect to a backend|rejected: confounds the measurement with an untested configuration|
|Quiesce the machine and drop the load column|cleaner numbers|the host is the maintainer's daily driver and runs container rebuilds; waiting for idle means baselines never get refreshed|rejected: record the confounder instead of pretending it is absent|

## Consequences

- **Positive**: every quoted figure carries its spread and its load, so a later
  run can tell a real regression from a noisy afternoon. The default-model cost
  is now a first-class row rather than an unmeasured assumption. `--dry-run`
  makes the exact command lines auditable without hardware.
- **Negative**: a full sweep costs roughly 4× the wall time of `bench_all.sh`
  (warmup + 3 reps), and the fixture set now needs a gitignored 4K pair and a
  gitignored YUV directory that a fresh worktree does not have — both are
  documented in
  [`docs/development/backend-perf-baselines.md`](../development/backend-perf-baselines.md).
- **Neutral / follow-ups**: `docs/benchmarks.md` keeps its historical rows and
  gains the refreshed ones; rows for backends that cannot currently complete a
  run are marked `BLOCKED` and cross-referenced to their `docs/state.md` entry
  rather than silently omitted, so an absent number is never mistaken for an
  unmeasured one.

## References

- Task direction: refresh the per-backend performance baselines for epic #1245,
  measuring the same fixture set across every backend this host can run, and
  record what the default model costs per backend now that its GPU twins work.
  Paraphrased from the dispatching instruction; the same instruction required
  that every number come from a command run on this machine, that each
  measurement be repeated at least three times and reported as median plus
  spread, that the load average during the run be stated, and that differences
  inside the noise be called out as such.
- [ADR-1169](1169-default-model-v1-0-16.md) — `vmaf_v1.0.16_3d0h` as the default model.
- [ADR-0700](0700-vmafx-repo-layout.md) — the `libvmaf/` → `core/` move that stale-dated the old reproduce block.
- [ADR-0517](0517-mcp-run-benchmark-repair.md) — the MCP consumer of `bench_all.sh`.
- [ADR-0845](0845-cuda-motion-launch-overhead.md) — the CUDA motion batching whose flush interaction blocks the GPU rows.
