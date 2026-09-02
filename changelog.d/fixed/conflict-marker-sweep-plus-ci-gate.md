### Fixed

- Remove committed conflict markers from 78 files introduced by the
  post-merge-train sweep commit (24bb5daf89). Markers kept the HEAD side
  in every case — the pre-ADR-0700-correct `core/` path references plus
  any forward-looking code additions (math import, `_sanitize_nonfinite`,
  `float_ansnr` removal note in test_hip_smoke.c, C++23 safety invariants
  in core/AGENTS.md).
- Add `check-conflict-markers` CI job to `lint-and-format.yml`: a fast,
  pre-commit-independent `git grep` gate that fails immediately on any
  committed conflict marker (`<<<<<<< ` / `=======` / `>>>>>>> `),
  blocking merges that bypass the local pre-commit hook.
