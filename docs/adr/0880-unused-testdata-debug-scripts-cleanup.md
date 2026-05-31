<!-- markdownlint-disable MD060 -->
# ADR-0880: Remove unreferenced testdata debug scripts and orphan snapshot

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: cleanup, testdata, repo-hygiene

## Context

A periodic audit of `model/` and `testdata/` for stale or vestigial files
found three fork-added artifacts in `testdata/` with zero in-tree references
outside themselves and no role in any documented workflow:

- `testdata/check_borders.py` — a one-off DWT-subband / ADM-border arithmetic
  debugging script written during the early SYCL-vs-CPU porting work. Computes
  border offsets for the 576×324 fixture at four scales and prints them. Used
  manually during initial SYCL ADM kernel development; never wired into any
  test, generator, or comparator. Output is reproducible from the formula in
  the ADM extractor itself.
- `testdata/compare_a380.py` — an early frame-by-frame comparator that diffs
  `scores_cpu_*.json` against `scores_sycl_a380_*.json`. Superseded by
  `testdata/compare_combined.py` (added in the same commit, but updated since
  to honour `VMAF_TESTDATA` overrides and resolve the repo root via
  `git rev-parse`). The two scripts duplicate the same comparison logic; only
  the newer one is referenced by tooling (`/run-netflix-bench` skill).
- `testdata/scores_sycl_b580_576_mq.json` — an orphan slim-schema snapshot
  for the B580 GPU at 576p with the `_mq` ("model-quality") suffix. The
  `run_sycl_scores.py` generator emits files of the form
  `scores_sycl_{gpu_tag}_{tag}.json`; the `_mq` suffix is non-standard and
  the file is never regenerated. Schema has 12 metrics versus the canonical
  34 (no `_num` / `_den` decomposition), confirming it was a one-off run
  with reduced output. Companion B580 snapshots at 1080/4k/576 (full schema)
  remain in place and are regenerated through the standard tag set.

The audit confirmed all `model/other_models/*.pkl`, `model/*rb_v0.6.*`, and
the predictor / tiny-AI registry entries are either upstream-mirrored
(must preserve per fork sync policy) or live registry / training artifacts.

## Decision

We will remove the three orphan artifacts listed above. The cleanup is
mechanical: no callers exist, no skill or test invokes them, and (for the
duplicate comparator) the surviving `compare_combined.py` is strictly more
capable.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Delete the three orphan files (chosen) | Removes 36 KB of dead weight, eliminates duplicated comparator logic, simplifies the audit surface for the next sweep | Loses one frame-by-frame debugger flavour — recoverable from git history if ever needed | Picked because the survivors strictly dominate and the audit gives a clean baseline |
| Leave the files in place | Zero risk of regret if someone later wants the slim B580 baseline or the older comparator | Continues to clutter the testdata listing; future agents repeat the same audit and arrive at the same conclusion | Rejected — every audit cycle pays the same cost for no gain |
| Move the files to a `testdata/attic/` directory | Preserves the artifacts without cluttering the active surface | Adds a directory that itself needs a policy (when does an attic file age out?); git history already serves this role | Rejected — `git log --all --follow` recovers any deleted file on demand |

## Consequences

- **Positive**: `testdata/` listing is shorter (32 entries vs 35);
  duplicated comparator logic is eliminated; future audits start from a
  cleaner baseline.
- **Negative**: A future debugger who wants to recompute ADM border
  offsets for 576×324 must recompute them inline or recover the deleted
  script from git history. The cost is small — the script is 42 lines of
  Python arithmetic.
- **Neutral / follow-ups**: None. No skills, tests, generators, or docs
  reference the removed files. The `/run-netflix-bench` skill continues to
  invoke `compare_combined.py` unchanged.

## References

- Audit task: ad-hoc agent dispatch 2026-05-30 (this PR).
- Related research: [docs/research/perf-snapshot-audit-2026-05-29.md](../research/perf-snapshot-audit-2026-05-29.md)
  documented the snapshot inventory and confirmed the `scores_sycl_*` /
  `scores_cpu_*` glob set used by `run_sycl_scores.py` does not include any
  `_mq` variant.
- Source: ad-hoc audit dispatch — `req` paraphrased: enumerate fork-added
  testdata + model files, triage by reference count, delete confirmed-unused
  files with strict ADR-0108 deliverables.
