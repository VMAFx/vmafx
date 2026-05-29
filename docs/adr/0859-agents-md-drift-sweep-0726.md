# ADR-0859: AGENTS.md drift sweep — ADR-0700 path rename and ADR-0726 Vulkan drop

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: Lusoris (user direction)
- **Tags**: docs, agents-md, maintenance, libvmaf-rename, vulkan-drop, fork-local

## Context

Two breaking changes accumulated staleness across all AGENTS.md files in the
fork:

1. **ADR-0700 (libvmaf/ → core/ rename, PR #1571)**: The C library root was
   renamed from `libvmaf/` to `core/`. Dozens of AGENTS.md files still
   carried path references, headings, and directory trees using the old
   `libvmaf/` prefix.

2. **ADR-0726 (Vulkan backend drop, PR #47)**: The Vulkan compute backend was
   removed from the fork. The rebase-invariant index in the root AGENTS.md
   §13 still listed Vulkan as an active invariant with multiple entries;
   per-backend AGENTS.md files still referenced Vulkan twins in their
   twin-update tables.

Additionally, `core/AGENTS.md` contained a committed merge-conflict marker
(lines 460–517) from a previous rebase, leaving a duplicate build-option
validation paragraph.

## Decision

Perform a mechanical sweep across all 44 AGENTS.md files:

1. Rename all `# AGENTS.md — libvmaf/...` headings to `# AGENTS.md — core/...`.
2. Update directory-tree blocks from `libvmaf/` to `core/`.
3. Fix all inline path references (`libvmaf/src/...`, `libvmaf/test/...`,
   `libvmaf/tools/...`) to their `core/` equivalents.
4. Remove the conflict marker in `core/AGENTS.md`, keeping the HEAD-side
   content (C++23 conversion invariants + `float_ansnr` removal invariant).
5. Replace the four stale Vulkan rebase-invariant entries in root AGENTS.md §13
   with a single "DROPPED (ADR-0726)" note.
6. Update twin-update tables in `core/src/feature/cuda/AGENTS.md` and
   `core/src/feature/sycl/AGENTS.md` to remove Vulkan columns.
7. Add deprecation headers to `core/src/vulkan/AGENTS.md`,
   `core/src/feature/vulkan/AGENTS.md`, and
   `core/src/feature/vulkan/shaders/AGENTS.md` (source files are still
   in-tree pending a cleanup PR).
8. Update `.github/AGENTS.md` macOS Vulkan-via-MoltenVK lane section with a
   REMOVED header.
9. Fix `dev/AGENTS.md` Vulkan backend probe and ICD references.

Non-mechanical changes (e.g., rewrites of invariant substance) are out of
scope for this sweep.

## Alternatives considered

- **No alternatives**: pure path-replacement and deprecation-note sweep with no
  behavioural decisions. The only judgement call was which Vulkan entries to
  remove outright vs. annotate as historical (chose annotate where the entry
  describes a CI lane or container config that may still need to be understood
  for rollback, remove outright where the entry was an active invariant that no
  longer applies).

## References

- ADR-0700: libvmaf/ → core/ rename
- ADR-0726: Vulkan backend drop
- PR #47: Vulkan backend removal commit
- Per user direction: "audit AGENTS.md files across the project for staleness
  post-ADR-0700 and other recent changes"
