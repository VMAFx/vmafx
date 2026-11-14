- CI: the pre-push PR-body-input selection test no longer reports a slow pass as a hang
  (issue #3457): exit 124 from the 12 s timeout counts as HUNG only when the gate never
  printed its `PR body from ...` decision line, and `ffmpeg-patches-surface-check.sh`
  takes a `--skip-invariants` flag in the test so the budget measures the fd-0 decision
  instead of the #2435 invariant tree scans, which run before the body stage and outran
  the budget under a parallel pre-push run. A gate blocked on fd 0 still prints nothing
  and is still reported as HUNG.
