**CI:** `docs.yml` now runs `mkdocs build --strict` on pull requests (same paths
filter as the push trigger), surfacing doc-substance gaps before merge rather than
after. The `deploy` job and `pages: write` permission remain push-only. Resolves
approximately 38 of the last 50 master-push failures caused by post-merge doc
build errors. See [ADR-0986](../docs/adr/0986-ci-docs-pr-trigger.md).
