---
paths:
  - scripts/run_unittests.sh
invariant: Upstream-mirror Netflix Python unittest runner; keep byte-identical on rebase.
---
<!-- markdownlint-disable MD013 MD038 -->
# Upstream-mirror unittest runner (`run_unittests.sh`)

## Rebase-sensitive invariants

### `run_unittests.sh` is upstream-mirror

Script = part of original Netflix Python test harness
(invokes `python3 -m unittest discover` against `python/test/`).
Keep byte-identical on rebase. Fork's CI uses meson +
pytest paths; file ships unchanged for upstream-sync hygiene.
