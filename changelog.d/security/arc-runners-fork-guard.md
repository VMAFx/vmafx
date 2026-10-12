- **A pull request from a fork never runs on the self-hosted `arc-runners` pool.** The Sanitizers
  and `Cppcheck (Whole Project)` jobs pick the pool only for a push, a dispatch, a schedule or a
  pull request from a branch of this repository; a fork's pull request runs on `ubuntu-26.04`
  whatever `ARC_RUNNERS_ENABLED` says. The `Tooling Tests` check refuses any other form of the
  selector ([ADR-3286](../../docs/adr/3286-arc-runners-fork-guard.md)).
