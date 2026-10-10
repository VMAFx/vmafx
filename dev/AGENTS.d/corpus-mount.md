---
paths:
  - dev/docker-compose.yml
  - scripts/ci/tests/test_dev_compose_corpus_mount.py
invariant: dev-mcp and smoke-probe-cron each bind ${VMAFX_CORPUS_DIR:-./.corpus} read-only at /workspace/.corpus.
---
<!-- markdownlint-disable MD013 -->
# Corpus mount

- `/workspace` = read-only bind of repo. `.corpus` in checkout possibly
  symlink to dataset outside repo (absolute path) -> dangling inside
  container. Separate bind: host resolves source; runc follows link inside
  container rootfs, so `/workspace/.corpus` reads mounted corpus.
- Both services carry bind: `source: ${VMAFX_CORPUS_DIR:-./.corpus}`,
  `target: /workspace/.corpus`, `read_only: true`,
  `bind.create_host_path: true` (host without corpus starts, empty dir).
  `test_dev_compose_corpus_mount.py` refuses missing, writable or
  hard-coded mount.
- Never add corpus to build context: `.dockerignore`, `.gitignore` keep
  excluding `.corpus`.
