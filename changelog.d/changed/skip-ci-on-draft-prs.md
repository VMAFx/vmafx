- **ci**: skip workflows on draft pull requests across all 8 fork
  workflows (`docker-image.yml`, `security-scans.yml`,
  `lint-and-format.yml`, `required-aggregator.yml`,
  `ffmpeg-integration.yml`, `libvmaf-build-matrix.yml`,
  `rule-enforcement.yml`, `tests-and-quality-gates.yml`). Each
  `pull_request` trigger now lists
  `types: [opened, synchronize, reopened, ready_for_review]` and every
  top-level job is gated on
  `github.event.pull_request.draft == false`. Push-to-master
  triggers are unchanged. Promotion of a draft PR via
  `ready_for_review` fires the full matrix; subsequent
  `synchronize` events on the now-ready PR fire CI as before. Cuts
  CI spend roughly in half against the fork's typical
  10+-draft-PR work-in-progress queue. See ADR-0331.
