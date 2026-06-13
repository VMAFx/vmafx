# PR-body deliverables validator (pre-push hook)

The fork's [rule-enforcement workflow][rule-yml] runs a **deep-dive
deliverables checklist** gate (ADR-0108) on every non-draft PR. The
parser is strict: a tick that does not match the documented checkbox
shape, or a label substring that drifts by one character, fails the
gate. Each retry costs a 3–10 minute CI cycle.

The pre-push hook described here mirrors that gate locally so a
malformed PR body is caught **before** the push.

## What it checks

The hook reuses the parser in
[`scripts/ci/deliverables-check.sh`][deliv] verbatim — that script is
the single source of truth for both CI and local validation. The
parser enforces:

1. **Six deliverables, each addressed.** For every item below, the body
   must contain either a ticked checkbox (`- [x] **Item name** …`) or
   an opt-out sentence (`no <key> needed: <reason>` /
   `no rebase impact: <reason>` / `no rebase-sensitive invariants`).

   | Item                              | Opt-out key regex                  |
   |-----------------------------------|------------------------------------|
   | Research digest                   | `digest`                           |
   | Decision matrix                   | `alternatives`                     |
   | `AGENTS.md` invariant note        | `rebase-sensitive\|AGENTS`         |
   | Reproducer / smoke-test command   | `reproducer\|smoke`                |
   | CHANGELOG fragment                | `changelog`                        |
   | Rebase note                       | `rebase`                           |

2. **Ticked items reference real files.** When a ticked item names a
   file class, the corresponding path must appear in the PR diff:

   | Ticked item       | Required diff entry                              |
   |-------------------|--------------------------------------------------|
   | Research digest   | `^docs/research/[0-9]+-`                         |
   | CHANGELOG fragment| `^CHANGELOG\.md` or `^changelog\.d/<sec>/.*\.md` |
   | Rebase note       | `^docs/rebase-notes\.md$`                        |

## Parser shape gotchas

The strict parser tripped PRs #461, #438, #470, #473, #486, #511, #468,
and #526 on these specific patterns:

- **Numbered-list shape fails.** `1. **Research digest** …` is not a
  checkbox — the parser only recognises `- [x]` (or `- [ ]`).
- **Bold-bracket label substring must be exact.** The label string
  inside the regex is matched case-insensitively but as a literal
  substring after markdown emphasis is stripped. `**Reproducer /
  smoke-test command**` matches; `**Reproducer / smoke-test**` (no
  trailing "command") does **not**.
- **Sentinel without un-tick is fine — but redundant.** A ticked box
  satisfies the gate on its own. The opt-out sentence is only required
  when the box is unticked. Mixing both is harmless.
- **Sentinel without ticked-OR-unticked checkbox is fine.** A bare
  sentence anywhere in the body satisfies the opt-out branch. The
  PR template still strongly recommends pairing it with `- [ ]` for
  reviewer legibility.

## Installing the hook

The hook ships as a tracked file at
[`scripts/git-hooks/pre-push`](../../scripts/git-hooks/pre-push). Wire
it into `.git/hooks/` via:

```bash
make hooks-install
```

The Make target installs the hook as a symlink (idempotent). Existing
non-symlink `pre-push` hooks are preserved with a `.local-backup`
suffix so a contributor's hand-rolled hook is never silently
overwritten.

## Standalone CLI

For one-off checks without installing the hook:

```bash
# Body via stdin, diff auto-computed from origin/master..HEAD
gh pr view 260 --json body -q .body \
  | scripts/ci/validate-pr-body.sh

# Explicit body file + explicit diff file
git diff --name-only origin/master..HEAD > /tmp/diff.txt
scripts/ci/validate-pr-body.sh --body pr-body.md --diff /tmp/diff.txt
```

Exit codes:

| Code | Meaning                                                  |
|------|----------------------------------------------------------|
| 0    | PR body would pass the deliverables gate.                |
| 1    | PR body would fail (same `::error` lines as CI emits).   |
| 2    | Usage error — missing body, unreadable diff file, etc.   |

## What the hook does on push

1. Resolves the current branch via `git rev-parse --abbrev-ref HEAD`.
2. Looks up the open PR for that branch via
   `gh pr view <branch> --json body,state,isDraft`.
3. **Skips silently** if any of the following hold (these mirror CI's
   own skip conditions, so they never produce a stricter gate than
   what CI runs):
   - `gh` is not installed,
   - the branch has no open PR (first push of a feature branch),
   - the PR is `MERGED` / `CLOSED`,
   - the PR is a draft (CI's `deep-dive-checklist` job has the same
     `pull_request.draft == false` predicate),
   - the PR body is empty,
   - `origin/master` is missing locally.
4. Otherwise, computes
   `git diff --name-only $(git merge-base origin/master HEAD)..HEAD`
   and feeds body + diff into
   `scripts/ci/validate-pr-body.sh`.
5. Non-zero exit blocks the push and prints the same `::error` lines
   the CI gate would emit.

## Bypassing

Standard escape hatch:

```bash
git push --no-verify
```

This skips **all** pre-push checks, not just this one. Use sparingly —
the most common legitimate reason is "the PR body is correct, but the
hook can't see it because `gh` auth is broken on this machine".

## Caveats — local pass is **not** a guarantee

This validator passing locally is not a guarantee that the CI gate
will pass. CI is authoritative for two reasons:

- **Diff source differs.** CI uses
  `git diff --name-only ${BASE_SHA}..${HEAD_SHA}` from the PR object;
  the hook uses `git diff --name-only $(git merge-base origin/master
  HEAD)..HEAD`. These usually agree but can diverge on stale local
  `origin/master` refs. Run `git fetch origin master` before relying
  on the hook.
- **Body source differs.** The hook fetches the body via `gh pr view`
  *as last saved on GitHub*. If you have unsaved edits in a local
  draft, the hook will validate the stale upstream body instead.

When the local validator and CI disagree, treat CI as the truth and
file the divergence as a bug against this script.

## How the parser works internally

Implementation summary (read [`deliverables-check.sh`][deliv] for the
exact regexes):

1. Strip markdown emphasis characters (`` ` *_\ ``) so labels wrapped
   in backticks/asterisks/underscores collapse to plain text.
2. For each of the six items, run two regex probes:
   - `- \[x\].*<item-name>` — case-insensitive, item-name as literal
     substring.
   - `no .*(<opt-out-key-regex>)` — case-insensitive.
3. If a ticked-only item references a file class, additionally probe
   the diff for the expected `^docs/research/`, `^changelog\.d/`,
   or `^docs/rebase-notes\.md$` paths.

Failure modes emit `::error title=ADR-0108 …::<message>` lines so the
GitHub Actions log surfaces them as inline annotations.

[rule-yml]: ../../.github/workflows/rule-enforcement.yml
[deliv]: ../../scripts/ci/deliverables-check.sh
