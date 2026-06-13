<!-- markdownlint-disable MD060 -->
# Research-0002: Automating process-ADR enforcement

- **Status**: Active
- **Workstream**: [ADR-0124](../adr/0124-automated-rule-enforcement.md)
- **Last updated**: 2026-04-20

## Question

Four process ADRs (0100 doc-substance, 0105 copyright, 0106
ADR-per-decision, 0108 deep-dive deliverables) rely on reviewer
discipline. What is the cheapest mechanism that surfaces violations
without drowning contributors in false-positive blocks?

## Sources

- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md) —
  per-surface doc bars.
- [ADR-0105](../adr/0105-copyright-handling-dual-notice.md) —
  three-template header policy.
- [ADR-0106](../adr/0106-adr-maintenance-rule.md) — one ADR per
  non-trivial decision, written *before* the commit.
- [ADR-0108](../adr/0108-deep-dive-deliverables-rule.md) — six
  deliverables per fork-local PR + the opt-out line syntax.
- [`.github/PULL_REQUEST_TEMPLATE.md`](../../.github/PULL_REQUEST_TEMPLATE.md)
  — carries the six-deliverable checklist; reviewer reads but no CI
  validates.
- Recent PRs #47 / #60 / #62 — all three ticked the full checklist
  voluntarily. Good evidence of baseline compliance, but zero
  evidence the system catches a PR that *didn't* comply.

## Investigation

### What's enforceable mechanically vs what isn't

| Rule | Can CI parse it? | Noise risk |
|---|---|---|
| ADR-0108 six-checkbox gate | Yes — PR body is structured Markdown; checkboxes are `- [ ]` / `- [x]`; opt-outs are `no <item> needed: <reason>` per ADR-0108 §Opt-out-lines | Low |
| ADR-0108 "referenced file exists in diff" | Yes — grep PR diff for `+.*docs/research/`, `+.*docs/rebase-notes.md`, `+.*CHANGELOG.md`; match if checkbox is ticked and no opt-out | Low |
| ADR-0105 copyright-header presence | Yes — `grep -q 'Copyright'` on each new `*.c/*.h/*.cpp/*.cu/*.cuh` | Low |
| ADR-0105 three-template correctness | Partial — regex can distinguish "Netflix only" / "Lusoris only" / "dual" but cannot know whether the file is fork-authored vs upstream-modified | Medium — use first-commit author as a heuristic; warn-only when ambiguous |
| ADR-0100 doc-substance | Partial — can flag PR-diff paths matching user-discoverable surfaces without `docs/` edits; cannot know whether the edit is a pure refactor (no behaviour change → exempt) | High — advisory comment only |
| ADR-0106 "ADR was written before the commit" | No — git history + PR description can show whether an ADR file was added, but not whether the author wrote it first. Best we can do is flag PRs that touch policy surfaces without adding an ADR | High — advisory comment only |

The split that falls out: ADR-0108 is the only rule whose *full*
predicate is mechanically checkable. The other three have at best
partial checks and should surface as advisory comments or pre-commit
warnings.

### Why `danger.js` and friends weren't chosen

`danger.js` / `danger-swift` would give nicer structured feedback
than raw shell, but:

- Adds a JS runtime to CI purely for this check. vmaf CI is
  C / Python / meson / bash; the `ubuntu-latest` base image already
  has `grep` and `gh`.
- Every node / npm transitive pin becomes a supply-chain surface
  (see [`supply-chain.yml`](../../.github/workflows/supply-chain.yml)).
- Structured feedback is nice-to-have; the signal is binary
  (deliverable present / absent). A 40-line bash script expresses
  this transparently.

### Why a single workflow instead of four

Per the user's validated preference on shared-surface refactors
(feedback memory: "for refactors in this area, user prefers one
bundled PR over many small ones"), and because the four gates share:

- the same trigger (`on: pull_request`)
- the same runner image (`ubuntu-latest`)
- the same `gh` CLI / `git` toolchain
- overlapping helper functions (parse PR body, list changed files)

Four separate workflow files would duplicate boilerplate and
fragment the mental model. One workflow, three jobs, one shared
action block.

### Opt-out syntax (ADR-0108 §Opt-out-lines)

ADR-0108 specifies that any of the six items may be skipped by
replacing the checkbox line with a justified opt-out:

- `no digest needed: trivial`
- `no alternatives: only-one-way fix`
- `no rebase-sensitive invariants`
- `no rebase impact: REASON`

The parser treats a line matching `/^-?\s*no .* (?:needed|impact)/`
on the position of an expected checkbox as a valid skip. This keeps
the rule flexible while still requiring the PR author to *say* why
the deliverable is absent.

## Dead ends / considered-and-rejected

- **Blocking `doc-substance-check`**: initial sketch blocked PRs
  that edited `core/src/feature/*.c` without a `docs/metrics/`
  diff. The `fix(libvmaf/feature): free VIF init base pointer`
  commit on PR #47 would have been blocked by this — a pure bug
  fix with no user-visible delta (the exemption in ADR-0100).
  The predicate "has user-visible delta" cannot be computed from
  the diff alone. Downgraded to advisory.
- **Commit-msg `git-hook` for ADR-0106**: would flag every
  "refactor:" / "chore:" commit as suspicious. False-positive rate
  too high. Moved to advisory PR comment.
- **Regex-matching the exact header template per file**: gets into
  a year-range detection rathole (the Netflix year range is itself
  a moving target — ADR-0105 currently pins `2016–2026`). Pre-commit
  hook checks for *any* `Copyright` line + flags missing-header
  cases; the reviewer handles template correctness.

## Verification plan

Before declaring the ADR Accepted:

1. Trigger the workflow on a synthetic PR that violates ADR-0108
   (un-ticked checkbox, no opt-out) — expect a red `deep-dive-checklist`
   status.
2. Trigger on a synthetic PR that adds a `*.c` file without a
   `Copyright` line — expect `pre-commit` to reject locally.
3. Trigger on a valid PR (pattern: the PR that ships this ADR itself)
   — expect all three jobs to pass + two advisory comments.

## Open follow-ups

- Narrow `adr-backfill-check` trigger paths if its advisory
  comment becomes noise (4-week burn-in window).
- Graduate `doc-substance-check` from advisory to blocking if the
  comment-to-fix turnaround holds up in practice.
