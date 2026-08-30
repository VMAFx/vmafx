- ARC self-hosted runner pool: pilot job for `Cppcheck (Whole Project)`
  in `lint-and-format.yml` now selects between `ubuntu-latest` and the
  in-cluster `arc-runners` scale set via the new repo variable
  `ARC_RUNNERS_ENABLED` (default `false`). See
  [ADR-0359](../docs/adr/0359-arc-runners-pilot.md) and
  [`docs/development/ci-runners.md`](../docs/development/ci-runners.md)
  for the operator playbook including the fall-back recipe when the
  cluster is degraded.
