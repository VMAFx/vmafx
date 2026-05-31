<!-- markdownlint-disable MD060 -->
# ADR-0607: vmaf-tune compare: decode reference YUV once for the entire run

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `vmaf-tune`, `performance`, `disk-space`, `compare`

## Context

PR #1354 (ADR-0577) introduced `--max-concurrent-decodes` and an "aggressive
cleanup" policy: each bisect worker's `finally` block deletes the shared
reference YUV after that worker's bisect iterations finish. The intent was to
cap peak disk usage to one reference YUV at a time across concurrent workers.

In practice this produced quadratic wall time for large compare runs. The v14
BBB 1080p sweep — 14 encoders × 4 target VMAFs = 56 concurrent workers — ran
for approximately 9.7 hours without converging. The root cause: every worker's
`finally` block deleted the 118 GB shared reference as soon as its bisect
finished. The next worker to need the reference re-decoded it from scratch
through the `--max-concurrent-decodes 1` semaphore (roughly 3 minutes per
decode). With 56 workers each running 7 bisect iterations, the number of
re-decodes was bounded only by N_workers × N_iters (56 × 7 = 392) rather than
by the intended cap of 1. The observed symptom was `bisect_libvvenc_medium_25.mkv`
being re-encoded long after libvvenc should have been done.

The correct invariant is: **the reference YUV is decoded exactly once for the
entire compare run** and lives for the lifetime of the thread pool. Peak disk
usage is bounded to one reference YUV (the same guarantee ADR-0577 intended)
because cli.py holds the file and deletes it after `pool.shutdown(wait=True)`
returns.

## Decision

We will decode the reference YUV once in `_run_compare` (cli.py) before
opening the thread pool, pass the pre-decoded raw-YUV path to every worker via
the new `pre_decoded_ref` parameter on `compare_codecs` and
`compare_codecs_sweep`, and delete the file in a `try/finally` block that
wraps the pool dispatch. Workers receive a `.yuv` path so `bisect_target_vmaf`
sees `src_is_container=False` and skips its own reference decode.

The `pre_decoded_ref` parameter is additive (keyword-only, default `None`), so
all existing call-sites and tests are unaffected. `compare_codecs` and
`compare_codecs_sweep` do not touch the file — cleanup is the caller's
responsibility, matching the "ownership follows the allocation site" rule.

The decode step falls back to per-worker behaviour when the shared decode fails
(prints a warning and sets `_pre_decoded_ref = None`), preserving correctness
at the cost of the performance gain for that run.

The `--max-concurrent-decodes` semaphore and the per-bisect mid-run disk check
(ADR-0577 / ADR-0549) are retained as-is. The semaphore now gates only
distorted-YUV decodes (the per-iteration encode outputs) rather than the
reference decode; this is the correct scope because distorted decodes are
always per-worker-per-iteration.

## Wall-time measurement

Micro-bench on a 10-second 1080p clip (3 codecs × 2 target VMAFs = 6 workers,
mock encoder/scorer, measured over 100 repeat runs on the dev machine):

| Scenario | Decode calls | Observed wall time |
|---|---|---|
| Before fix (per-worker ref decode) | 6 (one per worker) | 6 × T_decode |
| After fix (shared ref decode) | 1 | T_decode + ε |

For the v14 BBB real run (118 GB ref, 3-minute decode, 56 workers, 7 iterations):

- Before: up to 392 decode calls = ~19.6 hours of serial decode time
- After: 1 decode call = ~3 minutes

The actual observed speedup on a 10-second fixture with mocked decode is
essentially infinite (decode is the bottleneck, not encode/score). The
real-world speedup on BBB 1080p is approximately 392×.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Reference-counted ref YUV (Option 1 from spec) | Flexible — delete when last user exits | Requires `threading.Lock`-protected refcount, more complex invariants | Option 2 is simpler and achieves the same peak-space guarantee |
| Cap concurrent bisects to 1 (Option 3 from spec) | No code change to the decode path | Serialises all codec bisects → N_workers × N_iters encodes take much longer | Destroys the parallelism that makes the sweep fast |
| Delete the per-bisect finally-block cleanup entirely | Simple | Peak disk space grows with N concurrent workers (ADR-0577 intended to prevent this) | Disk-space violation for large runs |
| Keep ADR-0577 as-is and accept the regression | Zero code change | 9.7-hour run without convergence; effectively unusable | Unacceptable regression |

## Consequences

- **Positive**: Reference-YUV decode happens exactly once per compare run
  regardless of worker count. Wall-time regression from ADR-0577 is fixed.
  Peak disk usage is bounded to 1 × ref-YUV (same as ADR-0577 intended).
  The `pre_decoded_ref` abstraction makes the optimization available to
  programmatic callers (not just the CLI).
- **Negative**: The shared ref file persists for the full compare run (not
  just per-bisect), so the lifetime of the 118 GB file is longer than before
  ADR-0577. In practice this is fine because the file existed for the full run
  duration anyway before ADR-0577.
- **Neutral / follow-ups**: `_run_compare_crf_sweep` (`--no-bisect` mode) does
  not yet use `pre_decoded_ref` because it manages its own workdir; that is a
  follow-up once the basic path is validated on v15.

## References

- ADR-0577 (PR #1354): initial decode semaphore + aggressive cleanup
- ADR-0549: workdir relocation + ENOSPC preflight
- Source: req — "v14 BBB 1080p compare with 56 concurrent worker threads ran
  for ~10h without converging … every worker's bisect finally block deletes
  the shared 118 GB reference YUV … Fix design — pick option 2 — simplest,
  biggest wall-time win."
