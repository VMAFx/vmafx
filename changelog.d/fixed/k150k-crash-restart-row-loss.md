- `ai/scripts/extract_k150k_features.py` — fix silent row loss when
  a prior run was killed mid-write (Bug-3 RCA 2026-05-30). The
  restart no-op branch now compares the `.done` checkpoint against
  the on-disk parquet row count plus any recovered staging rows
  and raises `RuntimeError` on mismatch instead of confirming the
  loss with a `status=complete-noop` manifest. The end-of-run
  write path gains a row-accounting assert (recovered + ok must
  equal the row list length), an explicit `fsync` of the parquet
  file and its parent directory before the JSONL staging file is
  unlinked, and a stderr WARNING in `_load_staging_rows` reporting
  the count of malformed-JSON lines skipped (previously silent,
  masking truncated-tail crashes). See
  [ADR-0862](docs/adr/0862-k150k-crash-restart-row-loss-consistency-check.md).
