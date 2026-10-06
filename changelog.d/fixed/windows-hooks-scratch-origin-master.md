- **The `Windows Lefthook Pre-Commit` check no longer fails on every pull
  request.** The job runs the hooks in a scratch clone of the checkout, and a
  clone maps the checkout's local branches to `origin/*`. A pull-request
  checkout has no local `master`, so the always-run research-digest ID hook
  could not resolve `origin/master` and failed ("Needed a single revision"),
  which also turned the pull request's Required Checks Aggregator red. The
  step now copies the checkout's remote-tracking refs into the scratch clone.
