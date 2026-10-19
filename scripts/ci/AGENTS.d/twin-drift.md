---
paths:
  - scripts/ci/twin-drift-*
  - scripts/ci/tests/test-twin-drift-check.sh
invariant: Job name sits verbatim in aggregator; parser rules change in script and test together; awk stays POSIX.
area: tests
---
<!-- markdownlint-disable MD013 MD060 -->
# Twin-drift check

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `twin-drift-check.sh` + `twin-drift-allowlist.txt` | `lint-and-format.yml` — `twin-drift-check` job ([ADR-1135](../../../docs/adr/1135-ci-twin-drift-gate.md)); `twin-drift-check` pre-push hook in `.pre-commit-config.yaml` | job `name:` (`Twin Drift + Stale Source Refs (ADR-1135)`) is listed verbatim in `required-aggregator.yml` — rename both in same commit or every PR blocks on phantom check. allowlist path is default of `TWIN_DRIFT_ALLOWLIST`; each row is `<path> <reason>` and is validated (reason mandatory; row whose file is gone, whose side is compiled again, or whose pair no longer exists fails gate). source-extension regex (`c cpp cc cxx cu hip m mm metal pyx`), `output:` / `@…@` / absolute-path skip rules, `var + 'x.c'` and `os.path.join` resolution, suffix-search fallback and `twin-drift-ignore: <reason>` marker are parser contract — change them in script AND in `tests/test-twin-drift-check.sh` together. awk program must stay POSIX (mawk is Ubuntu's default `awk`): no `gensub`, no `length(array)`, no `--re-interval`-only syntax. |
| `tests/test-twin-drift-check.sh` | (local-only fixture driver, not invoked by CI) | Run before pushing changes to `twin-drift-check.sh`; 24 hermetic `mktemp -d` git-repo cases covering both predicates, allowlist validation and every resolution rule. Also run it under `gawk --posix` when touching awk. |
