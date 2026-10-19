---
paths:
  - scripts/ci/check-local-data-contract.sh
  - scripts/ci/tests/test-check-local-data-contract.sh
invariant: `.workingdir/` = private state, `.corpus/` = data, tracked files = public evidence; no tracked link into ignored data.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Local data-root separation (ADR-1277)

`check-local-data-contract.sh` separates three authorities: private state and
bounded cache under `.workingdir/`, datasets and reusable derived data under
`.corpus/`, and public evidence in tracked files. retired numbered state
root stays unignored so accidental recreation is visible. Tracked Markdown may
show local paths in commands but never link into ignored data. Preserve
hermetic shell suite, always-run pre-commit hook, and
`rule-enforcement.yml` invocation together.
