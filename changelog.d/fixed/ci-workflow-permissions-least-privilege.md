## Fixed

- **CI workflow permissions audit (ADR-1086)**: narrowed `upstream-watcher.yml`
  workflow-level permission from `read-all` to `contents: read`; the single job
  already opts into `issues: write` per-job so no behaviour changes. Added
  `persist-credentials: false` to the two `actions/checkout` steps in
  `docker-publish-operator-node.yml` (`build-operator`, `build-node`) that
  co-exist with `packages: write` scope, consistent with every other checkout
  in the repository.
