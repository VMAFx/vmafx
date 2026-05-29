# ADR-0813: testdata/ directory hygiene

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `testdata`, `gitignore`, `docs`, `benchmarks`, `fork-local`

## Context

The `testdata/` directory accumulated several hygiene issues over the first
batch of benchmark work:

1. `netflix_benchmark_results.json` and `perf_benchmark_results.json` — ad-hoc
   run outputs committed to the tree. CLAUDE.md §12 rule 5 explicitly prohibits
   committing benchmark output files unless the run is a formal baseline update.
   These were not gitignored, making accidental future commits easy.
2. `scores_sycl_b580_576_mq.json` — a snapshot produced by a one-off manual
   invocation. No generator script in tree produces it (the canonical
   `run_sycl_scores.py` does not emit a `_mq` suffix). It is unreferenced by
   any test, CI gate, or comparison script.
3. Three scripts (`test_all_backends.sh`, `bench_quick.py`, `compare_combined.py`)
   contained hardcoded absolute paths to `/home/kilian/dev/libvmaf_vulkan/`,
   the pre-rename working directory. These paths break on any other machine and
   in the dev container.
4. No `README.md` explaining what each file is, which are committed vs gitignored,
   and what belongs here vs `python/test/resource/yuv/` or `.corpus/`.

## Decision

- Delete `netflix_benchmark_results.json`, `perf_benchmark_results.json`, and
  `scores_sycl_b580_576_mq.json` from the tree.
- Add `.gitignore` entries for the two ad-hoc benchmark result patterns so they
  cannot be accidentally re-committed.
- Fix the three scripts to derive paths from `git rev-parse --show-toplevel` or
  `os.path.dirname(os.path.abspath(__file__))`.
- Add `testdata/README.md` documenting all committed files, their purpose, their
  consumers, and the regeneration workflow.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Keep ad-hoc JSONs, add comment | No deletion | Still commits stale machine data; violates CLAUDE.md §12 r5 | Violates the rule |
| Move large YUVs to `.corpus/` | Reduces repo size | YUVs are only 70 MB and actively used by CI test gate; `.corpus/` is gitignored and unavailable in clean clones | Breaks CI |
| Leave hardcoded paths as-is | No change | Breaks on any non-dev-machine; breaks in the `vmaf-dev-mcp` container | Not acceptable |

## Consequences

- **Positive**: clean tree, no machine-specific paths, ad-hoc outputs cannot
  be accidentally committed, new contributors can orient themselves via README.
- **Negative**: none — all deleted files were unneeded or regenerable.
- **Neutral**: `netflix_benchmark_results.json` is referenced in several docs
  as a historical data point; those references remain valid (they cite values,
  not the file's presence in the tree).

## References

- CLAUDE.md §12 rule 5 (no benchmark output commits).
- [ADR-0429](0429-testdata-bench-perf-portability.md) — `bench_perf.py` portability.
- [ADR-0752](0752-perf-bench-multi-resolution.md) — `perf_multi_resolution.json` baseline.
- Per user direction: "clean stale, open ready PR."
