## Upstream watchers fail closed on API errors (2026-10-15)

- Upstream watcher workflows (`.github/workflows/upstream-ffmpeg-hip-hwdec-watcher.yml`,
  `.github/workflows/upstream-netflix-645-hdr-model-watcher.yml`, and
  `.github/workflows/upstream-netflix-955-watcher.yml`) fail closed on API errors and search rate
  limits via `scripts/ci/upstream_watch_count.sh`, avoiding false positive issue creation when `gh api`
  outputs rate-limit JSON.
- Workflows idempotently ensure label `upstream-merged` exists before issue list and create calls.
- No rebase impact beyond these files and the `scripts/ci/` test and helper scripts.
