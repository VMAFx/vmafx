- **Two more PR-body gates hung when stdin was closed.** The first pass at
  `T-PR-BODY-EMPTY-STDIN-2026-09-16` routed
  `scripts/ci/deliverables-check.sh` and `scripts/ci/validate-pr-body.sh`
  through the new `scripts/ci/pr-body-input.sh`, but left the same
  `elif [ ! -t 0 ]; then / PR_BODY="$(cat)"` pair in the two sibling gates
  that had copied the idiom:
  `scripts/ci/ffmpeg-patches-surface-check.sh` (ADR-0186, the
  ffmpeg-patch surface gate) and `scripts/ci/state-md-touch-check.sh`
  (ADR-0165, the `docs/state.md` hygiene gate). Both still deadlocked on a
  closed `fd 0` — measured at `timeout 12 … 0<&-` → **124** (killed) for
  each — and both still labelled a `/dev/null` stdin as a body read "from
  stdin". Both now classify `fd 0` through the same shared helper. Their
  verdicts are unchanged for every shape that ever produced one: an absent
  body is still legal for these two, it merely makes their opt-out sentinel
  unclaimable, so they fall through to the diff check and fail closed
  instead of hanging. Also fixed in the helper: `pr_body_classify_stdin`
  handed the caller a duplicate of `fd 0` that nothing ever released, so
  every process a gate spawned afterwards — `git`, `python3`, `mktemp` —
  inherited it; the new `pr_body_close_stdin` releases it, and it cannot
  live inside `pr_body_read_stdin` because that runs in a `$( )` subshell
  whose close would not reach the caller. The regression suite
  `scripts/ci/tests/test-pr-body-input-selection.sh` grew from 24
  assertions to 39, covering all four gates and the descriptor release, and
  its pre-commit hook now triggers on all four gate files — the defect
  survived its first fix precisely because two of them were not listed.
  A second shortfall from that first pass is closed in the same change: making
  `deliverables-check.sh` source a sibling had left the pre-commit `shellcheck`
  hook red whenever that script was staged on its own, because the hook passes
  only the staged files and ShellCheck will not follow a `source=` directive to
  a file outside its input set. `scripts/ci/.shellcheckrc` now sets
  `external-sources=true` for that directory, which makes ShellCheck analyse
  the sibling rather than skip it. Measured over all 57 `scripts/ci/*.sh`
  checked one at a time: 4 findings before, 0 after, nothing new introduced,
  and no effect on the 120 shell scripts elsewhere in the tree.
