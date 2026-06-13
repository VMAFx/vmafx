`tools/external-bench/`: backfill unit-test coverage for the
benchmark orchestrator (85% → 99% line coverage on `compare.py`).
14 new tests exercise BVI-DVC and Netflix Public Drop discovery
edge cases (missing `ref`/`dis` dirs, empty refs, no geometry
token in folder/stem), `validate_wrapper_output()` rejection
paths (non-dict payload, non-list frames, non-dict frame,
non-integer/boolean `frame_idx`, boolean summary metric),
`run_wrapper()`'s missing-output guard, and the `main()`
`--limit` flag plus its per-item skip-and-continue path when a
wrapper fails. No production-code changes.
