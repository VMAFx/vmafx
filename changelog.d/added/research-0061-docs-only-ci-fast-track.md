- **Research-0061: docs-only PR CI fast-track design.** Tracks the
  docs-only / research-only PR pattern where a small markdown change
  waits ~25 minutes for the full 23-required-check CI matrix. Documents
  the working "shim + paths-filter detector" approach (the only one
  that satisfies GitHub branch protection's "skipped is not success"
  semantic), scopes it across the required-check inventory (18 of 23
  checks are skippable on docs-only diffs), and recommends a phased
  rollout starting with `libvmaf-build-matrix.yml`. No code change in
  this PR — design only; implementation deferred until the current
  merge train drains.
