---
paths:
  - scripts/ci/check-issue-reference-provenance.py
  - scripts/ci/tests/test_issue_reference_provenance.py
invariant: Protected historical contexts keep their archived `lusoris/vmaf` identity; contracts stay context-scoped.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Historical issue-reference provenance (BUG-048 Section E)

`check-issue-reference-provenance.py` protects small set of historical
issue and pull-request contexts proven to belong to retired
`lusoris/vmaf` tracker. active `VMAFx/vmafx` repository reused those
numbers for unrelated pull requests, so restoring bare issue number inside
one of protected contexts silently changes cited object. Keep
checker, its unit suite, always-run pre-commit hook, and Rule
Enforcement self-test wired together.

contracts are intentionally context-scoped: ordinary bare issue and PR
references normally mean active fork and must remain allowed, while
`Netflix/vmaf` tracker is separate upstream namespace. Add contract only
after Git history proves archived identity. Each contract uses stable
prose anchor and logical Markdown block; do not replace that with line
numbers, exact whitespace, repository-wide bare-reference ban, or network
lookup. See
[Research-2089](../../../docs/research/2089-archived-issue-reference-provenance.md).
