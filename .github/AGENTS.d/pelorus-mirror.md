---
paths:
  - .github/workflows/lint-and-format.yml
  - scripts/sync-pelorus-interop.sh
invariant: Pre-Commit job verifies exact declared commit of vidiko/pelorus before pre-commit; no moving branch or tag.
---
# Pelorus mirror verification stays in required Pre-Commit (ADR-1113, ADR-1276)

`Pre-Commit` job in `lint-and-format.yml` resolves full commit declared
by `scripts/sync-pelorus-interop.sh`, checks out `vidiko/pelorus` at that exact
object with credentials disabled, and runs default mirror/fixture drift
check. Keep this before `pre-commit --all-files`. Never change `ref` to
moving branch or tag, duplicate pin in workflow YAML, or tolerate missing
object: ABI-stable parser safety releases must be able to trigger reviewed
re-pin, and CI must prove source provenance rather than local-tree similarity.
