- **The praetor pin moves to `320b058d8cca`
  ([ADR-3337](docs/adr/3337-praetor-pin-320b058d-test-timeout.md)).** The praetor gate workflows
  use a concurrency group and check out without persisted credentials, the figure player lives
  under `tools/figures/player/`, and the Coverage Gate's timeouts above 30 minutes carry a
  three-day exception until its pytest step is split.
