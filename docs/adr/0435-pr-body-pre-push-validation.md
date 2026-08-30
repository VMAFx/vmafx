<!-- markdownlint-disable MD060 -->
# ADR-0435: PR-body pre-push validation hook

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ci`, `agents`, `hooks`, `docs`

## Context

The fork's `rule-enforcement.yml` workflow runs a deep-dive deliverables
checklist gate (ADR-0108) on every non-draft PR. Audit slice G
(`.workingdir/audit-2026-05-15/G-agents-hooks-skills.md`) found that 22
of 200 Rule Enforcement runs (11%) failed due to PR-body format errors,
consuming approximately 44 runner-hours of CI capacity. The dominant
failure mode was agent-generated PR bodies using prose bullet form
(`- Research digest: docs/research/foo.md`) instead of the required
checkbox form (`- [x] **Research digest** — docs/research/foo.md`).

`scripts/ci/validate-pr-body.sh` and `scripts/ci/deliverables-check.sh`
already existed and implemented the parser correctly. However, neither
was wired into any local hook. Contributors (and agents) had no mechanism
to catch format failures before pushing, so every malformed body triggered
a full CI cycle: Rule Enforcement itself takes 30–45 s, but the Required
Checks Aggregator polls for 90 minutes, making each failure cost roughly
120 minutes of CI wall time before the contributor could push a corrected
body.

## Decision

Wire `scripts/ci/validate-pr-body.sh` as a pre-push hook via three
complementary mechanisms:

1. **`scripts/git-hooks/pre-push-pr-body-lint.sh`** — a standalone,
   single-purpose script that implements the PR-body check. Referenced
   directly from `.pre-commit-config.yaml` as a `stages: [pre-push]`
   hook so that `pre-commit run --hook-stage pre-push` exercises it
   without running the omnibus `pre-push` script.

2. **`.pre-commit-config.yaml` entry** — `id: validate-pr-body`,
   `stages: [pre-push]`, `always_run: true`, `pass_filenames: false`.
   This integrates with the existing pre-commit infrastructure
   (`make hooks-install` already installs pre-commit hooks).

3. **`make pr-check` target** — already present in the Makefile; the
   ADR documents it as the canonical local run command for after
   `gh pr create`. The existing target is unmodified.

Additionally:

- `scripts/ci/deliverables-check.sh` is extended with anti-pattern
  detection: when the body contains prose bullet deliverable lines
  (`- Research digest: …`) without the corresponding checkbox form,
  a `::warning::` is emitted before the error so contributors
  (and agents) understand the format mismatch.

- `.github/PULL_REQUEST_TEMPLATE.md` receives a visible banner in the
  Deep-dive deliverables section explaining the checkbox requirement
  and opt-out syntax.

- `docs/development/pr-body-sentinel-guide.md` documents the six
  opt-out sentinel forms, ticked-file-reference checks, and the
  prose-vs-checkbox failure mode.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **A — Pre-push hook + pre-commit (chosen)** | Catches failures locally in <5 s before any CI cycle; composable with existing pre-commit infrastructure; no new dependencies | Requires `gh` CLI on PATH; drafts and no-PR pushes skip silently | Best total cost |
| **B — `make pr-check` only** | Zero-friction manual invocation | Requires contributor to remember to run it; does not block a push | Does not prevent the 44-runner-hour CI waste; contributors forget manual checks |
| **C — `check-pr-body` skill** | Could draft AND validate body before PR opens | Adds a skill file; relies on Claude Code context, not git hooks; does not catch non-Claude agent pushes | Incomplete coverage; hook covers all push paths including non-Claude agents |

## Consequences

- **Positive**: PR-body format failures are caught locally in under
  5 seconds, before any CI cycle starts. The 44 runner-hours of annual
  CI waste from prose-bullet bodies is eliminated for all contributors
  who have `make hooks-install` applied.
- **Positive**: The anti-pattern warning in `deliverables-check.sh`
  gives agents a clear, actionable message explaining why the gate is
  failing, reducing re-push iterations.
- **Positive**: The PR template banner makes the checkbox requirement
  visible to agents that generate bodies from the template without
  reading the surrounding documentation.
- **Negative**: Contributors without `gh` CLI installed skip the hook
  silently. CI remains the authoritative gate for those contributors.
- **Neutral**: The hook skips draft PRs, matching CI's
  `pull_request.draft == false` predicate, so it never blocks a
  draft-to-ready promotion push.
- **Neutral**: `make hooks-install` must be re-run by any contributor
  who cloned before this PR landed. The `pre-commit` hooks are
  auto-installed for new clones via the existing `make hooks-install`
  documentation in `docs/development/contributing.md`.

## References

- Audit slice G: `.workingdir/audit-2026-05-15/G-agents-hooks-skills.md`
  (§G.6, §G.7, §G.10, §G.11).
- ADR-0108: `docs/adr/0108-deep-dive-deliverables-rule.md`.
- ADR-0124: `docs/adr/0124-automated-rule-enforcement.md`.
- Related PRs that tripped the gate: #461, #438, #470, #473, #486,
  #511, #468, #526.
