- **CI queue**: Renovate now rebases its open pull requests only inside its
  weekly schedule and runs at most two pull requests at once, and every
  pull-request workflow cancels its older runs on a newer push; a master push
  no longer fills the runner queue with a rebase wave
  ([ADR-3384](../docs/adr/3384-ci-queue-burst-renovate-schedule.md)).
