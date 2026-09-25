# BUG-048 backend invariant restoration

## Finding

The implementation from three earlier fixes survived, but its rebase guidance
did not:

- commit `62b2103a9` paired CUDA module loads with module unloads, while the
  current CUDA feature guidance no longer stated that ownership rule;
- commit `53c8ef155` repaired HIP and Metal dispatch allowlists, while the HIP
  scope summary still called its live dispatch strategy a stub and neither
  backend guidance retained the exact-name coupling; and
- commit `4db777126` expanded `vmaf_pre` device parsing to all twelve
  `VmafDnnDevice` values, while its ADR-0482 rebase section was absent.

Current source inspection confirms all three implementations remain live.
`scripts/ci/check-dispatch-registry.sh` and its red-cap test pass, but that
checker covers extractor-symbol registration, not the separate HIP/Metal
string allowlists. The restored guidance states that boundary explicitly.
The FFmpeg patch stack is now pinned to `n9.0.2`, so the historical `n8.1`
replay command is intentionally not copied.

## Resolution

Restore the ownership and exact-name rules in the backend-local `AGENTS.md`
files and add a current ADR-0482 section to `docs/rebase-notes.md`. This is a
documentation correction: no runtime source, public header, patch content,
golden assertion, benchmark result, model, or training output changes.

## Alternatives considered

| Alternative | Benefit | Defect | Decision |
| --- | --- | --- | --- |
| Copy the historical text verbatim | Minimal archaeology | Restores retired paths, stale backend counts, and an obsolete FFmpeg tag | Rejected |
| Rely on source code alone | No documentation diff | Loses the ownership and cross-file coupling during later rebases | Rejected |
| Restate the invariant against current paths and gates | Accurate and actionable | Requires bounded source verification | Chosen |

No ADR is needed because no architecture or policy changes; this recovers
already-accepted maintenance contracts from surviving implementations.
