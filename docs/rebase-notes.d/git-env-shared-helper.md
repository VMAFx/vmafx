## Shared GIT_* environment scrub (2026-11-17)

`fix/issue-3324-git-env-helper`, [ADR-2197](adr/2197-render-generated-docs-at-landing.md). A rebase or upstream sync that conflicts across the migrated callers must keep the `git_env` import and whichever side's logic wins. Resolving a conflict back to an inline `GIT_*` filter is refused by `scripts/ci/test_git_fixture_isolation.py` at pre-commit time, so the scrub cannot quietly fragment again.
