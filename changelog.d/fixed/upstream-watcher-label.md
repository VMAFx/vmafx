- **The upstream-watcher workflow creates its tracking label before filing issues.**
  `upstream-watcher.yml` failed when `upstream-blocked` was missing from the repository.
  Both jobs now call `scripts/upstream-watcher/ensure_label.sh` to ensure the label
  exists idempotently, and a new `scheduled-failure` guard job reports scheduled runs
  that fail using `scripts/ci/scheduled-failure-issue.sh`.
