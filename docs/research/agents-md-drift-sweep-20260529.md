# Research digest: AGENTS.md drift sweep (2026-05-29)

**Status**: Complete
**ADR**: ADR-0859
**Scope**: Documentation maintenance, no code changes

## Summary

Audited all 44 `AGENTS.md` files across the fork for staleness following two
breaking changes:

1. **ADR-0700** (`libvmaf/` → `core/` rename): Found 50+ instances of the
   old path prefix across 20 files. All mechanical path substitutions
   completed.

2. **ADR-0726** (Vulkan backend drop): Found Vulkan references still active
   in 12 files. Key changes:
   - Root AGENTS.md §13: 4 Vulkan rebase-invariants replaced with a single
     "DROPPED" note.
   - Twin-update tables in `core/src/feature/cuda/AGENTS.md` and
     `core/src/feature/sycl/AGENTS.md`: Vulkan columns removed.
   - `core/src/vulkan/`, `core/src/feature/vulkan/`, and
     `core/src/feature/vulkan/shaders/` AGENTS.md files: deprecation
     headers added (source files are still in-tree pending cleanup).
   - `.github/AGENTS.md` macOS Vulkan-MoltenVK lane: marked REMOVED.
   - `dev/AGENTS.md`: container probe args and ICD references updated.

3. **Conflict marker**: `core/AGENTS.md` contained a committed conflict
   marker at lines 460–517 from a prior rebase. Resolved by keeping the
   HEAD-side content (C++23 conversion invariants + `float_ansnr` removal
   invariant from ADR-0720).

## Findings

- No non-mechanical changes required (no missing invariants, no substance gaps).
- The Vulkan source files under `core/src/vulkan/` and
  `core/src/feature/vulkan/` are still tracked in git (the ADR-0726 PR added
  the ADR doc and changelog fragment but did not execute the file deletions).
  A follow-up cleanup PR is needed to actually remove those ~89 files.
- The `dev/AGENTS.md` Vulkan ICD configuration notes were annotated rather than
  removed, because the container entrypoint still sets these env vars and a
  future operator may need to understand why.
