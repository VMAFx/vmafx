# K150K extractor crash-restart row-loss RCA (2026-05-30)

<!-- Copyright 2026 Lusoris -->

## Summary

`ai/scripts/extract_k150k_features.py` reported 152 265 completed clips
in its `.done` checkpoint while the on-disk parquet held only 59 812
rows — a silent loss of approximately 92 K rows discovered on
2026-05-30 during a routine integrity check of the K150K feature
corpus.

The root cause is a missing post-condition in the restart no-op
branch: when `pending == []` the script copies any recovered staging
rows into the parquet and returns `status=complete-noop` without ever
verifying that the resulting row count matches `.done`. A prior run
killed mid-write (parquet partially flushed but staging file already
unlinked, or rename(2) reordered ahead of data on a non-fsync'd
filesystem) leaves the parquet truncated and the staging file gone;
every subsequent invocation silently confirms the loss.

The fix is three coordinated guards plus an `fsync` ordering fix.

## Failure-mode table

| # | Symptom | Where | Root cause | Fix |
| --- | --- | --- | --- | --- |
| 1 | Silent JSONDecodeError on staging-tail truncation | `_load_staging_rows` L865–879 (pre-fix) | `continue` on exception, no count surfaced | WARNING with skipped-line count + recovered count |
| 2 | Restart no-op branch confirms `.done`-vs-parquet mismatch | `main` L1310–1329 (pre-fix) | No row-count comparison; manifest written `status=complete-noop` regardless | Raise `RuntimeError` when `len(done_set) > parquet_rows + recovered` |
| 3 | End-of-run row accounting unchecked | `main` L1405–1408 (pre-fix) | `rows` is built from two sources (recovered + as_completed) with no cardinality assert | Assert `len(rows) == len(recovered_rows) + ok`; raise + preserve staging on mismatch |
| 4 | Staging unlinked before parquet is durable | `main` L1408 (pre-fix) | `staging_path.unlink()` runs immediately after `_write_parquet_from_rows` returns; no `fsync` of parquet or parent dir between rename(2) and unlink(2) | New `_fsync_path` helper called before `staging_path.unlink` in both no-op and end-of-run paths |

## Reproducer

```bash
# Synthesize the failure mode
mkdir -p /tmp/k150k-rca
cd /tmp/k150k-rca

# 100-entry .done checkpoint
printf 'clip_%04d.mp4\n' {0..99} > k150k.done

# Parquet with 50 rows (the "lost half")
python -c "
import pandas as pd
pd.DataFrame([{'clip_name': f'clip_{i:04d}.mp4', 'vmaf': 95.0}
              for i in range(50)]).to_parquet('k150k.parquet')
"

# No staging file — operator-cleaned

# Run the script (pre-fix: prints 'nothing to do.' and exits 0
# with status=complete-noop in the manifest)
# Run the script (post-fix: raises RuntimeError with CONSISTENCY ERROR
# message naming the 50-clip gap and the recovery hint)
```

The post-fix behaviour is pinned by
`ai/tests/test_extract_k150k_consistency.py`:

```bash
pytest ai/tests/test_extract_k150k_consistency.py -v
# 4 passed in 0.25 s
```

## In-flight run guidance

The K150K extraction currently running (PID 307050, 98.3 % complete as
of 2026-05-30, ETA ~1.5 h) must complete before this fix lands. The
fix changes only the *restart* path's behaviour — once the in-flight
run finishes its at-end parquet write, the operator can rebase + merge
this PR; the new consistency check will succeed because the freshly
completed parquet matches `.done`.

If the in-flight run *also* crashes mid-write before the fix lands,
the operator will hit the existing silent-loss antipattern one more
time. Acceptable: the next restart attempt after this PR lands will
surface the deficit instead of confirming it.

## References

- Source: `req` (operator RCA sketch + fix plan, 2026-05-30 13:54 UTC)
- ADR: [ADR-0862](../adr/0862-k150k-crash-restart-row-loss-consistency-check.md)
- Related: Research-0135 (parquet write-once-at-end optimisation — preserved)
- Test: `ai/tests/test_extract_k150k_consistency.py`
