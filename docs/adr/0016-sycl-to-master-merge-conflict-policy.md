<!-- markdownlint-disable MD013 MD060 -->
# ADR-0016: Sycl to master merge conflict resolution policy

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: git, workspace

## Context

Step 6 of the Phase 1 integration (`sycl → master`) had many conflicts because both branches had diverged for months over CI config, README, build files, and SYCL/CUDA/SIMD additions. Blind merges would either lose fork work or undo upstream fixes. A file-class policy is needed.

## Decision

We will resolve merge conflicts with: fork-side wins for CI/README/build/SYCL-CUDA-SIMD files; upstream wins for metric code not touched by the fork; manual merge for `core/include/libvmaf/libvmaf.h`, `core/src/libvmaf.c`, and `core/meson.build`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `-X ours` everywhere | Fast | Loses upstream metric fixes | Unacceptable |
| `-X theirs` everywhere | Fast | Destroys fork additions | Unacceptable |
| File-class policy (chosen) | Predictable; preserves both sides | Requires judgement on edge cases | Correct compromise |

This decision was a default — blanket strategies were both wrong.

## Consequences

- **Positive**: predictable merge outcomes; documented handling for the three high-conflict files.
- **Negative**: reviewer must verify the three manual-merge files carefully.
- **Neutral / follow-ups**: `/sync-upstream` skill encodes the policy.

## References

- Source: `Q4.4`
- Related ADRs: ADR-0002
