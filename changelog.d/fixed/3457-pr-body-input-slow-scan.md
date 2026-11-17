- **Stop the PR-body-input selection test reporting a slow surface-gate pass as a hang.**
  The four #2435 invariant tree scans run before the PR-body stage in
  `ffmpeg-patches-surface-check.sh` and, under a parallel pre-push run or on a slow
  checkout, alone outran the selection test's `timeout 12`, so a killed-but-working gate
  was reported as `HUNG (killed at 12 s); the gate must decide, not block` while a
  standalone rerun passed (issue #3457). The gate takes a `--skip-invariants` flag that
  skips only those tree scans — the PR-body classification, the diff-set build and the
  intersection verdict run exactly as a full pass — and the selection test passes it in
  every surface case, so the 12 s budget measures the fd-0 decision instead of the scans;
  CI never passes the flag, which the gate's python suite pins against every workflow,
  composite action, `.pre-commit-config.yaml` and `lefthook.yml`. `run_case` keeps the
  hang verdict honest: a 124 whose output carries the gate's `PR body from ...` line
  decided fd 0 inside the budget, so the case is re-run once with a 60 s budget and
  judged on its verdict — a slow case that completes passes, a second timeout is still
  HUNG — and a 124 without the line never reached the decision and is HUNG outright.
  Planted cases keep the verdict armed: a gate blocked on a silent stdin (portable), one
  that prints the decision line and then blocks, and, where the platform hangs instead
  of reporting EBADF, the exact pre-fix closed-fd-0 `PR_BODY="$(cat)"` repro.
