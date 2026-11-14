---
paths:
  - api/run-result-timeline.schema.json
  - api/testdata/run-result-timeline/*.json
  - scripts/ci/tests/test_run_result_timeline_schema.py
invariant: Timeline schema grows additively (HISS-14); remove, rename, retype or new required member = new schema_version.
area: tests
---
<!-- markdownlint-disable MD013 -->
# Run-result timeline schema contract (ADR-3538)

- `api/run-result-timeline.schema.json` version 1 only grows. Removed, renamed or retyped member, new required member, narrower enum or changed `schema_version` contract -> new schema version, never edit of version 1.
- Rebase conflict that drops or retypes schema member = contract break, not cleanup: keep schema side, reopen discussion (`docs/rebase-notes.d/run-result-timeline-schema.md`).
- Fixtures under `api/testdata/run-result-timeline/` change with schema in same PR. `valid-*` validate, `invalid-*` fail; `test_run_result_timeline_schema.py` checks both sets.
