- **The first-release candidate plan moved back by one candidate.**
  `v1.0.0-rc.2` is a stabilisation candidate: it ships the dependency updates
  and fixes merged since rc.1, and testers use the same report kit
  (`tools/rc1-tester/`) and exit bar as for rc.1. Benchmarking, profiling and
  tuning move to `v1.0.0-rc.3`, and the one-shot model retrain moves to
  `v1.0.0-rc.4`, so each phase number now matches its tag. The release guide,
  roadmap, tester guide, retrain runbook and `vmaf-rc1-report list-tools`
  inventory show the new mapping (ADR-1352).
