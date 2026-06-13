### Fix nightly-bisect CI failure: tracker pointed at closed PR instead of a standalone issue

`nightly-bisect.yml` used `BISECT_TRACKER_ISSUE: "40"`, which resolved to PR #40
(a closed pull request), not a standalone GitHub issue. The `post-bisect-comment.py`
script posts the sticky result comment via the GitHub Issues comments API; the
GITHUB_TOKEN with `issues: write` can post to standalone issues but not to closed
pull requests. The workflow had been failing every night since 2026-05-29.

Fix: created standalone tracking issue #827 ("tracking: nightly bisect-model-quality
results") and updated `BISECT_TRACKER_ISSUE` to `"827"`. Also improved
`_gh_with_stdin` in `post-bisect-comment.py` to surface `gh` stderr on failure,
making future API errors diagnosable in CI logs.
