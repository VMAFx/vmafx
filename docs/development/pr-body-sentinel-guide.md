<!-- markdownlint-disable MD060 -->
# PR body sentinel guide

The fork's `rule-enforcement.yml` workflow runs a **deep-dive
deliverables checklist** gate (ADR-0108) on every non-draft PR.
The parser is strict: a tick that does not match the documented
checkbox shape, or an opt-out that uses the wrong sentinel word,
fails the gate. Each retry costs a 3–10 minute CI cycle.

The pre-push hook (`scripts/git-hooks/pre-push-pr-body-lint.sh`,
wired by `make hooks-install`) and the standalone validator
(`scripts/ci/validate-pr-body.sh`) run the same parser locally
before the push so failures are caught in under 5 seconds.

## Quick start

```bash
# Install the pre-push hook (idempotent):
make hooks-install

# Run the validator manually against an open PR:
make pr-check PR=<number>

# Run against a local body file:
make pr-check BODY=pr-body.md

# Run the raw validator:
gh pr view <number> --json body -q .body \
  | scripts/ci/validate-pr-body.sh
```

## The six deliverables and their opt-out forms

Every fork-local PR body must address each of the six deliverables
below. Each item may be addressed either by:

1. **Ticking the checkbox** (`- [x] **Item name** — …`), or
2. **Opting out** with a sentinel sentence anywhere in the body.

Upstream-port PRs (`/port-upstream-commit`) and pure upstream syncs
are exempt from the entire section.

| Deliverable | Required checkbox label | Opt-out sentinel key | Example opt-out |
|---|---|---|---|
| Research digest | `Research digest` | `digest` | `no digest needed: trivial change` |
| Decision matrix | `Decision matrix` | `alternatives` | `no alternatives: only-one-way fix` |
| `AGENTS.md` invariant note | `AGENTS.md invariant note` | `rebase-sensitive` or `AGENTS` | `no rebase-sensitive invariants` |
| Reproducer / smoke-test command | `Reproducer / smoke-test command` | `reproducer` or `smoke` | `no smoke-test needed: doc-only` |
| CHANGELOG fragment | `CHANGELOG fragment` | `changelog` | `no changelog needed: internal refactor` |
| Rebase note | `Rebase note` | `rebase` | `no rebase impact: docs-only` |

### Exact syntax

**Ticked checkbox (item addressed):**

```text
- [x] **Research digest** — docs/research/0435-foo.md written.
```

**Opt-out (item explicitly skipped):**

```text
- [ ] **Research digest** — no digest needed: trivial change.
```

The sentinel phrase (`no digest needed:`) may appear anywhere in the
body — inside the checkbox line or as a standalone sentence. The parser
strips markdown emphasis characters (backticks, asterisks, underscores)
before matching, so label wrapping does not affect detection.

## Ticked-file-reference checks

When an item is ticked, the parser also verifies that the
corresponding file type appears in the PR diff. Missing the file
after ticking the box fails the gate even if the checkbox shape
is correct.

| Ticked item | Required diff entry |
|---|---|
| Research digest | `docs/research/NNNN-*.md` (any file matching `^docs/research/[0-9]+-`) |
| CHANGELOG fragment | `CHANGELOG.md` OR `changelog.d/<section>/<topic>.md` |
| Rebase note | `docs/rebase-notes.md` |

If you tick "Research digest" but do not add a `docs/research/NNNN-*.md`
file, the gate fails with:

```text
::error title=ADR-0108 research digest::Checkbox ticked but no
docs/research/NNNN-*.md added in this PR.
```

Fix: either add the digest file, or untick the box and write
`no digest needed: <reason>` instead.

## The prose-bullet failure mode (most common agent mistake)

The parser requires **checkbox** syntax (`- [x]`). Prose bullets
and numbered lists are **not** recognised.

| Format | Parser result |
|---|---|
| `- [x] **Research digest** — docs/research/0435-foo.md` | PASS |
| `- [ ] **Research digest** — no digest needed: trivial` | PASS (opt-out) |
| `- Research digest: docs/research/0435-foo.md` | FAIL — prose bullet |
| `1. **Research digest** — docs/research/0435-foo.md` | FAIL — numbered list |
| `**Research digest**: docs/research/0435-foo.md` | FAIL — no checkbox |

When the gate detects a prose bullet, it emits a warning before the
error line:

```text
::warning title=ADR-0108 prose-bullet format::One or more deliverables
  appear to be in prose bullet format (e.g. '- Research digest: ...').
  The parser requires the checkbox form: '- [x] **Research digest** ...'
```

## Local run command

```bash
# After gh pr create or gh pr edit, validate the saved body:
make pr-check PR=<number>

# Against a local draft file before opening the PR:
make pr-check BODY=.workingdir/pr-batch-0-body.md
```

Exit codes:

| Code | Meaning |
|------|---------|
| 0 | PR body would pass the deliverables gate |
| 1 | PR body would fail (same `::error` lines as CI emits) |
| 2 | Usage error — missing body, unreadable diff file, etc. |

## Bypassing the pre-push hook

The standard escape hatch skips **all** pre-push checks:

```bash
git push --no-verify
```

Use this only when the PR body is correct but the hook cannot validate
it (for example, `gh` authentication is broken on the current machine).

## See also

- [ADR-0108](../adr/0108-deep-dive-deliverables-rule.md) — the
  six-deliverable rule.
- [ADR-0435](../adr/0435-pr-body-pre-push-validation.md) — the
  decision to wire this as a pre-push hook.
- [pr-body-validator.md](pr-body-validator.md) — implementation
  internals (parser shape, shim design, caveat on local-vs-CI
  divergence).
- [`scripts/ci/deliverables-check.sh`](../../scripts/ci/deliverables-check.sh) —
  the single source of truth parser.
- [`.github/PULL_REQUEST_TEMPLATE.md`](../../.github/PULL_REQUEST_TEMPLATE.md) —
  the template that carries the checklist.
