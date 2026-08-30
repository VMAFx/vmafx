- Second dependency batch: the remaining Renovate PRs after the 36-PR sweep —
  `pandas-stubs`, `anthropic`, `ray[tune]` (SECURITY), the grouped Docker digest
  refresh, GitHub Actions minor/patch, and `onsi/gomega`. Merged as one branch
  with one CI run for the same reason as the first batch (ADR-1123): the merge
  gate cannot drain a per-PR fan-out of the full macOS/ARM/CUDA/SYCL matrix for
  one-line manifest edits.
- Note the Docker entry arrived pre-grouped as a single PR rather than one per
  image — that is the `groupName: "Docker digests"` rule from ADR-1123 taking
  effect, which was the point of adding it.
