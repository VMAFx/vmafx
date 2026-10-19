---
paths:
  - dev/scripts/dev-mcp-entrypoint.sh
  - dev/Containerfile
invariant: Entrypoint runs as `vmaf`; it changes path's mode or owner only if path is its own and change is needed.
---
<!-- markdownlint-disable MD013 -->
# The entrypoint is unprivileged

- image's `USER` is `vmaf` (uid 2000) and entrypoint runs under
  `set -euo pipefail`. `chmod` / `chown` on root-owned path fails with
  EPERM and ends container (restart loop, compose reports it unhealthy).
- Guard every such call: test first (`stat -c %a`, `[ -O path ]`), act only
  when mode is wrong and path is ours, or end line with
  `2>/dev/null || true` when failure is acceptable.
- Why it matters: uutils coreutils (Ubuntu 26.04 base) changed between 0.8
  and 0.10. 0.8 skipped `chmod` that would not change mode; 0.10
  issues it. unconditional `chmod 1777 /tmp` worked for months and broke
  with base digest update of #1799.
- Check after base image change: start rebuilt image with
  `docker compose -f dev/docker-compose.yml up -d dev-mcp` and wait for
  `healthy`; build that succeeds says nothing about entrypoint.
