- **The ADR-0108 PR-body gates hung when stdin was closed, and read
  `/dev/null` as an empty PR description.** `scripts/ci/deliverables-check.sh`
  and `scripts/ci/validate-pr-body.sh` still selected their input with
  `[ ! -t 0 ]`, which answers "is fd 0 a terminal" rather than "did anybody
  pipe a PR body". Two shapes fell in that gap. Run with fd 0 closed
  (`bash scripts/ci/deliverables-check.sh 0<&-`) the gates did not fail —
  they **deadlocked**: `PR_BODY="$(cat)"` opens a command-substitution pipe,
  the kernel hands out the lowest free descriptor, with fd 0 free that pipe's
  read end lands on fd 0, and `cat` then reads the pipe it is writing to.
  Measured at 124 (killed) under `timeout 10`, unbounded without one. Run
  with stdin on `/dev/null` — the shape a CI step, a git hook and `nohup` all
  produce — the gates read an empty body and reported "the PR description is
  empty", blaming the author for a producer's fault. Both scripts now
  classify fd 0 before touching it, through the shared
  `scripts/ci/pr-body-input.sh`: a pipe, regular file or socket is read; a
  terminal, a closed descriptor and `/dev/null` are each named in a usage
  error (exit 2). The classifier duplicates fd 0 up front, which is what
  makes reading it safe. A `PR_BODY` that is *set* now counts as the
  caller's answer even when blank, so an empty `github.event.pull_request.body`
  is reported against the env var instead of falling through to stdin and
  arriving mislabelled. Every supported invocation is unchanged: piped body,
  `--body PATH`, `$PR_BODY`, and the `gh`-fetched bodies behind
  `make pr-check` and the pre-push hook. Covered by 24 new assertions in
  `scripts/ci/tests/test-pr-body-input-selection.sh`, wired as a
  pre-commit/pre-push hook and bounded by `timeout` so a re-regression
  reports a hang rather than becoming one.
