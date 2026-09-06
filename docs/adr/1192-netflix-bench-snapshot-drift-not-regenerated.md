<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1192: Keep the recorded Netflix benchmark snapshot; do not regenerate it while the GPU paths are broken

- **Status**: Accepted
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: benchmark, cuda, sycl, testdata, docs

## Context

`testdata/netflix_benchmark_results.json` is the fork's recorded per-backend
score-and-throughput snapshot for the three Netflix fixtures (576x324 src01
pair, 1080p checkerboard mild, 1080p checkerboard heavy). It was last written
by PR #309 on 2026-05-02 and is produced by `testdata/benchmark_netflix.py`
driving an FFmpeg build that links the fork's `libvmaf`, `libvmaf_cuda` and
`libvmaf_sycl` filters.

Re-running the suite on `cd52f2670` (2026-09-06) reproduced all three fixtures
on CPU and CUDA and — after correcting a stale VA-API render-node assumption in
the harness — on SYCL as well. Every backend's pooled score has drifted from the
recorded values, and two independent GPU defects surfaced during the run:
the `vmaf` CLI aborts on **any** GPU backend as soon as `--threads` is passed,
and the `libvmaf_cuda` FFmpeg filter intermittently emits wrong per-frame
scores. Both were shown by rebuilding `5a080300e` (the commit immediately
before the 2026-09-06 GPU merges #1307, #1312 and #1324) that they predate
those merges.

The `/regen-snapshots` rule (CLAUDE.md §9) requires an explicit, committed
justification for rewriting these snapshots, and ADR-0024 forbids papering over
a numeric delta. A snapshot rewritten now would bake the CUDA flakiness into the
recorded "expected" values and destroy the only committed record of what the
backends scored before the drift.

## Decision

We will leave `testdata/netflix_benchmark_results.json` exactly as recorded, and
gate its regeneration on the two GPU defects being fixed. The measured drift is
recorded as data — in `docs/development/netflix-benchmark-baselines.md` and in
`docs/state.md` rows — not written back into the snapshot. Because the scores do
not match, no fresh throughput baseline is written into `docs/benchmarks.md`
either: a timing table recorded next to a non-reproducible CUDA score would be
misleading. The harness itself is made honest and portable in the same change
(stderr preserved, exit codes not relabelled as "backend unavailable", VA-API
render node and FFmpeg path overridable by environment).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Regenerate the snapshot now | Makes the diff green; records today's numbers | Bakes a 20–25 % flaky CUDA row in as "expected"; destroys the pre-drift record; violates the `/regen-snapshots` justification bar | The CUDA row is not reproducible run-to-run, so there is no single correct value to record |
| Delete the CUDA rows from the snapshot | Removes the flaky data | Removes a user-visible record without an accepted decision to drop the surface | Removing a surface to make a gate pass is exactly what the fork forbids |
| Loosen the comparison tolerance | One-line change | Hides a 1.4e-3 SYCL and 1.1e-3 CUDA pooled delta and a 25 %-of-runs wrong-score defect | Test-weakening; ADR-0024 |
| Keep the snapshot, record the drift, fix the harness (**chosen**) | Preserves the audit trail; surfaces two real defects; harness becomes reproducible off this host | Leaves a visibly stale snapshot in tree until the GPU work lands | The stale-but-honest state is cheaper than a fabricated-fresh one |

## Consequences

- **Positive**: the pre-drift per-backend numbers stay in git; the two GPU
  defects are now backed by committed reproducers instead of a static
  call-graph argument; the harness runs on a host where the Arc is not
  `renderD130` and where `/home/kilian/dev/ffmpeg-8` does not exist.
- **Negative**: `testdata/netflix_benchmark_results.json` stays knowingly stale,
  and `docs/benchmarks.md` keeps its 2026-05 throughput table. Anyone diffing
  against either has to read `docs/development/netflix-benchmark-baselines.md`
  first.
- **Neutral / follow-ups**: regeneration is unblocked once
  `T-GPU-CLI-THREADS-CTX-SYNC-2026-09-06` and
  `T-CUDA-FFMPEG-FILTER-NONDETERMINISM-2026-09-06` close. The regenerating PR
  must cite this ADR and re-measure all three fixtures on all three backends.

## References

- `testdata/benchmark_netflix.py`, `testdata/bench_all.sh`,
  `testdata/netflix_benchmark_results.json`
- [ADR-0024](0024-netflix-golden-preserved.md) — golden data is not edited to
  make a gate pass
- [ADR-0792](0792-hardcoded-yuv-path-env-overrides.md) — environment overrides
  for hard-coded host paths in `testdata/` harnesses
- [ADR-0429](0429-testdata-bench-perf-portability.md) — portability precedent
  for `testdata/bench_perf.py`
- `docs/state.md` row `T-UPSTREAM-1305-CUDA-DRAIN-BATCH-THREAD-GLOBAL-2026-09-03`
  — the statically-derived hypothesis these runs give empirical weight to
- Related: epic #1245 (items 1 and 5), PRs #309, #1307, #1312, #1324
- Source: `req` — paraphrased: the maintainer asked for the Netflix benchmark
  suite to be re-run on current master and the baselines recorded, with a score
  delta treated as a correctness finding rather than a reason to regenerate the
  snapshot.
