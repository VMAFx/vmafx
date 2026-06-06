- `tools/vmaf-tune`: add 33 parametrized subprocess-mock CLI tests for
  `encode-profile`, `compare`, and `report` sub-commands
  (`tests/test_cli_subcommands.py`). Covers dry-run and live-encode paths,
  all text-based format variants (markdown/json/csv/html/both), schema-v1 and
  schema-v2 compare-JSON ingestion, ladder-JSON and per-shot-JSON ingestion,
  and every documented error path (missing geometry, empty encoder list,
  bad JSON files, unavailable-encoder vs real-failure `ok`/`degraded` flags).
  No real ffmpeg or vmaf binary is invoked.
