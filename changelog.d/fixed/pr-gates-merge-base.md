- **Pull request gates judge only the pull request's own change.** The deliverables, `docs/state.md`
  and FFmpeg-patch surface gates diffed from `BASE_SHA`, the base branch tip when the event fired, so a
  pull request that fell behind master was judged on every file master changed since: the deliverables
  gate refused it for the rendered `CHANGELOG.md` and ADR index (ADR-2197), and the state gate could
  pass it on a row master added. They now diff from the merge base of `BASE_SHA` and `HEAD_SHA`
  (`scripts/ci/pr-diff-base.sh`) and fail closed when there is none; the dependency classifier and the
  release-PR exemption no longer fall back to the base tip either.
