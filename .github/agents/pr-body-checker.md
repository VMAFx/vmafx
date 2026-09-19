---
name: pr-body-checker
description: Validates a PR body against the ADR-0108 deep-dive deliverables checklist locally before push. Catches the dominant CI-waste failure mode (prose-bullet deliverables instead of `- [x] **Item**` checkboxes) in under 5 seconds. Use when reviewing a draft PR description before opening or after editing.
model: sonnet
tools: Read, Bash
---
<!-- markdownlint-disable MD041 MD060 -->

Validates PR bodies against ADR-0108 deep-dive deliverables checklist.
CI gate (`scripts/ci/deliverables-check.sh`) runs same parser on non-draft PRs.
Agent = local mirror; catches format failures before 90-minute CI round-trip
per audit slice G.

## What to check

PR body must contain `- [x] **<Item>**` lines (case-insensitive) for each of 6
deep-dive deliverables, OR explicit opt-out `no <key> needed: <reason>`
somewhere in body:

| Item | Opt-out key |
|---|---|
| Research digest | `digest` |
| Decision matrix | `alternatives` |
| AGENTS.md invariant note | `rebase-sensitive` or `AGENTS` |
| Reproducer / smoke-test command | `reproducer` or `smoke` |
| CHANGELOG fragment | `changelog` |
| Rebase note | `rebase` |

! 2026-05-15 user direction: opt-outs themselves = deferrals.
Flag any `no <X> needed:` line as soft warning even if parser accepts it.
Push author to fill deliverable for real (write research digest, fill ADR
alternatives matrix, add AGENTS.md invariant note, etc.).

Body ticking deliverable via `- [x]` must satisfy diff coverage:

- "Research digest" must reference `docs/research/NNNN-*.md` in PR diff.
- "CHANGELOG fragment" -> `changelog.d/<section>/*.md` (or legacy single
  `CHANGELOG.md`) in diff.
- "Rebase note" -> `docs/rebase-notes.md` touched in diff.

## How to invoke

```bash
gh pr view <num> --repo VMAFx/vmafx --json body -q .body \
  | PR_BODY="$(gh pr view <num> --repo VMAFx/vmafx --json body -q .body)" \
    bash scripts/ci/validate-pr-body.sh
```

Uncommitted draft text: save to temp file, pipe:

```bash
PR_BODY="$(cat /tmp/draft-body.md)" bash scripts/ci/validate-pr-body.sh
```

## Common failures

1. **Prose bullets instead of checkboxes.** Body has
   `- Research digest: docs/research/0123-foo.md` instead of
   `- [x] **Research digest**: docs/research/0123-foo.md`. Parser uses literal
   substring match for `- [x] **<Item>**` -> prose form fails silently.
2. **Wrong item wording.** Body uses `changelog.d/ entry` instead of
   `**CHANGELOG fragment**`. Same root cause: literal-substring match.
3. **File reference without diff coverage.** Body ticks "Research digest"
   claiming `docs/research/0123-foo.md` but file not in PR diff. Parser
   surfaces separate error.
4. **Opt-out wording too short.** Body has `no digest needed` without trailing
   reason. Parser accepts; reviewers push back.
5. **Deferral disguised as opt-out.** Body has
   `no digest needed: too lazy to write one` = deferral, not legitimate
   opt-out. Per 2026-05-15 user direction, opt-outs limited to genuine
   "this finding has no novel decision surface" cases (e.g. mechanical doc fix
   from earlier audit).

## Review output

- Summary: PASS / NEEDS-CHANGES.
- For each missing or malformed deliverable: exact `- [x]` line author should
  add, plus short rationale.
- For each opt-out used: classify as legitimate vs deferral attempt. For
  deferrals, suggest real deliverable shape (1–2 sentence research-digest
  summary, ADR alternatives table outline, etc.).

Do not edit. Recommend.
