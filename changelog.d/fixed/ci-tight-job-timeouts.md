- **Three required lint checks could be cancelled before doing any
  work.** `ShellCheck + shfmt` ran with `timeout-minutes: 1`, and
  `No Conflict Markers` and `Twin Drift` with 2. The first is a no-op
  proxy that just echoes a line; the other two do a checkout. None of
  them needs anywhere near that long to *run* — but the budget also has
  to cover acquiring a runner, and a large push fans out dozens of
  workflows at once and saturates the pool. The rc.1 train's 159-commit
  fast-forward to master did exactly that: `ShellCheck + shfmt` sat for
  six minutes, executed zero steps, and was cancelled, turning the
  README's Lint badge red on a commit whose lint was clean. All three
  are required checks, so the same failure would block an ordinary PR
  on a busy day. They now get 5 minutes, matching `Markdown Lint`; a job
  that finishes in seconds costs nothing extra.
