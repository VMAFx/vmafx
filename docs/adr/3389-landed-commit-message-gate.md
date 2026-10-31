<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3389: Landed commit message gate and bot squash sign-off

- **Status**: Accepted
- **Date**: 2026-10-30
- **Deciders**: Lusoris (ledger vmafx-coauthor-gate-20261030)
- **Tags**: `ci`, `git`, `dco`, `renovate`, `fork-local`

## Context

When GitHub executes a squash merge without an explicit commit body, it generates a
default body composed of the individual commit messages from the pull request branch.
If a pull request branch contains commits from multiple contributors (such as bot
updates followed by human edits or multiple bot steps), GitHub appends `Co-authored-by:`
trailers for every contributing author to the squashed commit.

On master `f035ad03e` (#3042), a Renovate automerge landed with bot `Co-authored-by:`
trailers because a maintainer commit on the bot branch made the pull request multi-author;
`d5af73342` (#3006) is the same defect from the render job. A replay of `--landed` over the
400 master commits before this record refuses 116, which is why the job judges only new
pushes and no history is rewritten. Renovate's platform automerge option (`platformAutomerge: true`)
delegates merge execution to the forge using `automergeCommitMessage`, splitting at the first
newline into headline and commitBody. Without an explicit `commitBody`, the forge synthesises
the default squash body. Similarly, `scripts/ci/open-render-pr.sh` invoked `gh pr merge --auto
--squash` without passing `--body-file`, exposing automated doc renders to the same issue.

The project requires Developer Certificate of Origin (DCO) sign-off on every landed commit
without automated tool or bot attribution trailers, consistent with fleet standards.

## Decision

1. Configure `renovate.json` with top-level `"commitBody": "Signed-off-by: {{{gitAuthor}}}"`.
   Renovate appends this commit body to its commit message and passes it to the forge
   automerge API call, ensuring that squash merges use this body and omit `Co-authored-by:`
   trailers.
2. In `scripts/ci/open-render-pr.sh`, pass an explicit `--body-file` containing the render
   commit body and the bot's `Signed-off-by:` trailer in both auto-merge calls via a helper
   function `arm_auto_merge`. Compute bot identity prior to the first merge call.
3. Extend `scripts/ci/check-dco.py` with `--landed` mode and `--landing-identity`. In
   `--landed` mode:
   - All exemptions (bot logins, `--release-pr`, and grandfathering cutoff) are disabled.
   - Every non-merge commit must contain a sign-off trailer matching either the commit author
     address or the designated landing identity address (`--landing-identity`). The committer
     address is not accepted because GitHub is the committer on every squash merge.
   - Message lines are scanned for disallowed tool and bot `Co-authored-by:` trailers
     (including claude, anthropic, openai, chatgpt, gpt, codex, gemini, copilot, cursor,
     opencode, antigravity, and `[bot]`).
   - Message lines are scanned for machine-generated attribution phrases and robot emojis.
   - If the base SHA is all zeros (initial push) or an object not reachable from the head,
     only the head commit is judged.
4. Add `.github/workflows/landed-commit-messages.yml` running on pushes to `master` as a
   second barrier to alarm if an invalid commit message lands on `master`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Extend `check-dco.py` with `--landed` (chosen) | Reuses existing DCO trailer parsing and validation logic; single script for commit sign-off rules (HISS-19); zero code duplication | Adds a mode switch to `check-dco.py` | — |
| Copy Praetor's `commit_msg_check.py` into the repository | Independent script without touching the pull request check | Duplicates commit inspection logic, git parsing, and sign-off regex (HISS-19 violation) | A second implementation is a defect under HISS-19 |
| Enforce as a required PR check only | Fails before landing on master | Cannot inspect the post-squash commit message that GitHub creates during the squash merge | The squash body is generated at merge time by GitHub; PR checks cannot inspect the final squash commit |

Praetor (Latin for magistrate, the fleet standards engine) enforces commit message rules
across repositories in the fleet. Extending `check-dco.py` applies the same policy while
maintaining single-source implementation hygiene.

## Consequences

- **Positive**: bot squash commits land on `master` with verified sign-offs and without
  tool or bot `Co-authored-by:` trailers; the second barrier on `master` immediately alerts
  on any malformed landed commit messages.
- **Negative**: an invalid commit that lands on `master` triggers a red check on `master`;
  because published git history is immutable, the issue must be resolved by correcting the
  automation configuration rather than rewriting history.
- **Neutral / follow-ups**: human co-authors remain permitted on `master` when accompanied
  by a valid sign-off; all future automerging tools must provide explicit squash bodies.

## References

- [ADR-2462](2462-dco-sign-off-required.md) Developer Certificate of Origin sign-off required.
- [ADR-3295](3295-render-only-pull-request.md) and [ADR-3373](3373-scheduled-render-pull-request.md) render
  pull request automation.
- Renovate 44.161.2 `dist/`: `config/options/index.js:2562` (`commitBody`),
  `workers/repository/update/branch/index.js:448-449` and `:474-479`,
  `modules/platform/github/index.js:1183-1190`. GitHub CLI `pkg/cmd/pr/merge/http.go:72-74`
  passes `--body-file` to the auto-merge mutation.
- Praetor commit message enforcement standards.
