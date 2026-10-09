# PR-body deliverables validator (pre-push hook)

To check a PR body before pushing, run `make pr-check PR=<number>` or install
the pre-push hook with `make hooks-install`. Both run the same parser as the
CI gate, so a malformed body is caught locally instead of after a 3-10 minute
CI cycle.

The [rule-enforcement workflow][rule-yml] runs a **deep-dive deliverables
checklist** gate (ADR-0108) on every non-draft PR. The parser is strict: a
tick that does not match the documented checkbox shape, or a label substring
that drifts by one character, fails the gate. For the user-facing syntax
(checkbox forms, opt-out sentences) read the
[PR body sentinel guide](pr-body-sentinel-guide.md); this page covers the
validator's internals and the hook.

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

These patterns have each failed real PRs under the strict parser:

- **Numbered-list shape fails.** `1. **Research digest** …` is not a
  checkbox — the parser only recognises `- [x]` (or `- [ ]`).
- **Bold-bracket label substring must be exact.** The label string
  inside the regex is matched case-insensitively but as a literal
  substring after markdown emphasis is stripped. `**Reproducer /
  smoke-test command**` matches; `**Reproducer / smoke-test**` (no
  trailing "command") does **not**.
- **A sentence next to a ticked box is redundant.** A ticked box
  satisfies the gate on its own. The opt-out sentence is only required
  when the box is unticked. Mixing both is harmless.
- **Sentinel without ticked-OR-unticked checkbox is fine.** A bare
  sentence anywhere in the body satisfies the opt-out branch. The
  PR template still strongly recommends pairing it with `- [ ]` for
  reviewer legibility.

## Installing the hook

Run `make hooks-install` (an alias of `make install-hooks`,
[ADR-1241](../adr/1241-worktree-hook-dispatch.md)). It needs `pre-commit` in
the active Python environment and runs
[`scripts/githooks/install.py`](../../scripts/githooks/install.py), which:

1. Validates `.pre-commit-config.yaml` and prepares the hook environments.
2. Writes the dispatcher `scripts/githooks/dispatch.sh` into `.git/hooks/` as
   `pre-commit`, `commit-msg`, `pre-push` and `pre-rebase` (a copy, not a
   symlink).
3. Preserves any existing managed hook next to it as
   `<hook>.vmafx-backup-*`, and refuses to touch a custom hook it does not
   own.

The PR-body check is the `validate-pr-body` hook in
[`.pre-commit-config.yaml`](../../.pre-commit-config.yaml) (stage
`pre-push`). Its entry point is
[`scripts/git-hooks/pre-push-pr-body-lint.sh`](../../scripts/git-hooks/pre-push-pr-body-lint.sh);
the dispatcher reaches it through the pre-commit framework.

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

## Where the body comes from

Four gates read a PR body, and all four classify `fd 0` the same way,
through the shared helper [`scripts/ci/pr-body-input.sh`][input]:

| Gate                              | Rule enforced                    |
|-----------------------------------|----------------------------------|
| [`deliverables-check.sh`][deliv]  | ADR-0108 six deliverables        |
| `validate-pr-body.sh`             | the same parser, run locally     |
| `ffmpeg-patches-surface-check.sh` | ADR-0409 ffmpeg-patch surface    |
| `state-md-touch-check.sh`         | ADR-0165 `docs/state.md` hygiene |

The body is resolved in this order:

1. `--body PATH` (`validate-pr-body.sh` only).
2. `$PR_BODY`. For the two deliverables entry points, **set counts** — even
   to the empty string, because setting it is the caller's answer, so a blank
   one is reported against the variable rather than quietly replaced from
   stdin. This is the path CI takes
   (`PR_BODY: ${{ github.event.pull_request.body }}`) and the path
   `make pr-check` takes. The surface and `state.md` gates instead take the
   variable only when it is **non-empty**: for them a blank `$PR_BODY` and a
   blank stdin end at the same empty body, so falling through costs nothing
   and lets an explicitly blank variable still be overridden by a real pipe.
3. stdin, **when fd 0 is a pipe, a regular file or a socket**.

What happens when none of those supplies a body differs by gate, because the
gates differ in what else they have to go on:

- `deliverables-check.sh` and `validate-pr-body.sh` exist only to parse a
  body. No body is a **usage error (exit 2)** naming what fd 0 actually is.
- `ffmpeg-patches-surface-check.sh` and `state-md-touch-check.sh` also have a
  diff to check. No body makes their opt-out sentinel unclaimable, so they
  **name what fd 0 was and fall through to the diff** — passing when it is
  clean, failing when it is not. Neither is weakened by an absent body; both
  fail closed.

The classification matters because `[ ! -t 0 ]` — the test all four scripts
used until it was replaced — answers "is fd 0 something other than a
terminal", not "did anybody pipe a PR body":

| fd 0                    | `[ ! -t 0 ]` | Now                                          |
|-------------------------|--------------|----------------------------------------------|
| pipe / file with a body | true         | read (unchanged)                             |
| pipe carrying no bytes  | true         | fail closed: the producer sent nothing       |
| `/dev/null`             | true         | named as unreadable, never read as a body    |
| closed (`0<&-`)         | true         | named as closed, never read — used to hang   |
| terminal                | false        | named as a terminal (unchanged)              |

### Why fd 0 is classified

The closed case was the sharp one. `PR_BODY="$(cat)"` with fd 0 closed does
not fail, it **deadlocks**: the command substitution opens a pipe, the
kernel hands out the lowest free descriptor, with fd 0 free that pipe's
read end lands on fd 0, and `cat` reads the pipe it is writing to. Reproduce
the old behaviour on any pre-fix checkout with:

```bash
timeout 10 bash scripts/ci/deliverables-check.sh 0<&- ; echo $?            # 124
timeout 12 env -u PR_BODY bash -c \
    'bash scripts/ci/state-md-touch-check.sh 0<&-' ; echo $?               # 124
```

### Reading the classified stream

Reading the classified stream is a three-step contract, and the third step is
not optional:

1. `pr_body_classify_stdin` hands out a **duplicate** of fd 0. The duplicate
   makes the read safe, since no later command substitution can claim a
   descriptor that is already taken.
2. `pr_body_read_stdin` reads the duplicate.
3. `pr_body_close_stdin` releases it.

The release cannot be folded into the read. The read runs inside `$( )`, so
closing there would close the subshell's copy and leave the caller's open,
inherited by every `git`, `python3` and `mktemp` the gate spawns afterwards.

`scripts/ci/tests/test-pr-body-input-selection.sh` pins every row of that
table for all four gates, plus the descriptor release, under `timeout`, so a
re-regression is reported as a hang instead of becoming one.

## What the hook does on push

1. Resolves the current branch via `git rev-parse --abbrev-ref HEAD`.
2. Looks up the open PR for that branch with a bounded `gh pr view`
   (timeout `VMAFX_PR_LOOKUP_TIMEOUT_SECONDS`, default 15 s). If `gh` is not
   installed or fails, it falls back to the public GitHub pull-request pages.
3. **Fails closed** when neither source can establish the PR state: the push
   is blocked with `cannot determine PR state via gh or GitHub's public
   pages`. Restore connectivity or credentials and push again.
4. **Skips** (exit 0, with a message) in these cases, which mirror CI's own
   skip conditions so the hook is never stricter than CI:
   - the branch has no open PR (first push of a feature branch),
   - the PR is `MERGED` / `CLOSED`,
   - the PR is a draft (the `deep-dive-checklist` job has the same
     `pull_request.draft == false` predicate),
   - the PR is the machine-generated release-please PR (ADR-1151; needs
     `gh` metadata, the public-page fallback never exempts),
   - the PR is a strictly dependency-only bot PR, such as a Renovate update of
     `go.mod` and `go.sum` (ADR-1152: `scripts/ci/classify-dependency-pr.sh`
     with the author, head ref and changed files CI passes it; needs `gh`
     metadata, the public-page fallback never exempts),
   - the PR body is empty (CI will catch it),
   - `origin/master` is missing locally (run `git fetch origin master`).
5. Otherwise computes
   `git diff --name-only $(git merge-base origin/master HEAD)..HEAD` and
   feeds body and diff into `scripts/ci/validate-pr-body.sh`.
6. A non-zero exit blocks the push and prints the same `::error` lines the CI
   gate would emit.

## Bypassing

Do not skip the hook. AGENTS.md operational rule 6 forbids agents from
evading hooks, and the PR body is checked again in CI.

If `gh` authentication is broken, the public-page fallback usually still
reads the PR. When it cannot, fix connectivity or credentials and retry.

!!! note
    Only a human, in their own terminal, may skip pre-push checks with
    git's standard skip flag, and that skips **all** pre-push checks, not
    just this one. Correct the PR body (`gh pr edit --body-file <path>`)
    instead whenever possible.

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
[input]: ../../scripts/ci/pr-body-input.sh

## History

The strict parser tripped PRs #461, #438, #470, #473, #486, #511, #468
and #526 on the shape patterns listed under "Parser shape gotchas". The fd 0
classification was added after a closed `fd 0` deadlocked all four body-reading
gates.
