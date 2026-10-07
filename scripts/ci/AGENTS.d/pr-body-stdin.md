---
paths:
  - scripts/ci/pr-body-input.sh
  - scripts/ci/deliverables-check.sh
  - scripts/ci/validate-pr-body.sh
  - scripts/ci/ffmpeg-patches-surface-check.sh
  - scripts/ci/state-md-touch-check.sh
  - scripts/ci/.shellcheckrc
  - scripts/ci/tests/test-pr-body-input-selection.sh
invariant: Four gates source `pr-body-input.sh` from their own directory: classify stdin, read, close; never test `[ ! -t 0 ]`.
---
<!-- markdownlint-disable MD013 MD060 -->
# PR-body stdin classification (`pr-body-input.sh`)

**Invariant — hard sibling-file dependency.** Four scripts source
`scripts/ci/pr-body-input.sh` unconditionally, before any input branch, each
resolving it from its own `${BASH_SOURCE[0]}` directory rather than from
`$PWD`: `deliverables-check.sh`, `validate-pr-body.sh`,
`ffmpeg-patches-surface-check.sh`, `state-md-touch-check.sh`. None of four
is standalone any more. Consequences to preserve:

- Copying, vendoring or relocating one of those scripts **must** carry
  `pr-body-input.sh` with it, into same directory. copy that loses
  sibling does not degrade — it dies at `.` line before it reads anything.
- `pr-body-input.sh` is sourced, never executed, and defines only
  `pr_body_*` functions and `PR_BODY_STDIN_KIND` / `PR_BODY_STDIN_FD`
  variables. Do not give it top-level side effects: it runs inside four gates
  that have already set `set -euo pipefail` and their own `trap … EXIT`.
- rebase-sensitive surfaces table above lists these scripts against their
  workflow jobs. workflow that invokes one of them by path is invoking two
  files; checkout or artifact that ships only named script is broken.
- `scripts/ci/.shellcheckrc` sets `external-sources=true` **because** of this
  dependency. pre-commit `shellcheck` hook passes only staged files, so
  staging one of four without helper made ShellCheck emit SC1091 ("not
  specified as input") on file that is fine. `external-sources` makes it
  follow `# shellcheck source=` directives four already carry — more
  analysis, not less: measured over all 57 `scripts/ci/*.sh` checked one at
  time, 4 findings before and 0 after, with nothing new introduced and no
  effect on 120 shell scripts outside this directory. Keep directive
  lines when editing these scripts, and do not reach for
  `# shellcheck disable=SC1091` instead.

**Invariant — never test `[ ! -t 0 ]` for "was a body piped".** That asks
whether fd 0 is terminal, which is true for `/dev/null`, for CI step's
null stdin, and for *closed* descriptor. On closed fd 0
`PR_BODY="$(cat)"` does not fail — it **deadlocks**, because command
substitution's pipe takes freed descriptor 0 and `cat` reads pipe it
is writing to. All four gates hung this way; measured at `timeout 12` → 124.
New gates that read PR body call `pr_body_classify_stdin` instead.

**Invariant — classify, read, close.** `pr_body_classify_stdin` hands
caller *duplicate* of fd 0 in `PR_BODY_STDIN_FD`; duplicate is what
makes subsequent read safe, since no later command substitution can claim
descriptor already in use. caller must release it with
`pr_body_close_stdin` after reading. That release cannot move inside
`pr_body_read_stdin`: callers invoke it as `"$(pr_body_read_stdin)"`, and
`exec {fd}<&-` in subshell closes subshell's copy while caller's
stays open — inherited by every `git`, `python3` and `mktemp` gate spawns
afterwards.

**Invariant — absent body means different things to different gates.**
`deliverables-check.sh` / `validate-pr-body.sh` exist only to parse body, so
no body is exit 2. `ffmpeg-patches-surface-check.sh` /
`state-md-touch-check.sh` also have diff, so no body makes their opt-out
sentinel unclaimable and they fall through to diff check. Do not
"harmonise" two families: turning latter pair into exit 2 makes
missing body abort gate instead of enforcing it. They deliberately also
differ on `$PR_BODY` precedence — `+x` (set counts) for first pair, `-n`
(non-empty) for second, because only first pair reports two cases
differently.

`scripts/ci/tests/test-pr-body-input-selection.sh` pins all of above for
all four gates, bounded by `timeout` so re-regression reports hang rather
than becoming one. It is wired as pre-commit/pre-push hook whose `files:`
pattern lists every gate; adding fifth caller means adding it there too.
