<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1096: Doxygen @brief/@param coverage for core internal headers

- **Status**: Accepted
- **Date**: 2026-06-07
- **Deciders**: Lusoris maintainers
- **Tags**: `docs`, `maintainability`

## Context

`core/src/*.h` internal headers (not shipped as public API) have no Doxygen
coverage. 214 internal headers exist; only 26 had any `@brief`/`@param`
annotation before this change. The ten highest-traffic headers — those
included by the most `.c` files and covering the engine's primary
abstractions (framesync, thread pool, picture pool, prediction, feature
collection, ref-counting, memory, logging, options, dictionary) — had zero
maintainer documentation. New contributors and agents authoring GPU/SIMD
extensions have to read both the header and its `.c` implementation to
understand preconditions, ownership, and error semantics.

## Decision

Add Doxygen `@brief`, `@param[in/out]`, and `@return` comments to the ten
core internal headers with the highest cross-file inclusion frequency:
`framesync.h`, `thread_pool.h`, `picture_pool.h`, `predict.h`,
`fex_ctx_vector.h`, `ref.h`, `mem.h`, `log.h`, `opt.h`, and `dict.h`.
No public API headers are touched (those already have Doxygen coverage or
are out-of-scope for this round). Changes are purely additive comments;
no logic is modified.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Document all 188 undocumented headers in one PR | Complete coverage | >2000 LOC, difficult to review, high noise risk | PR LOC cap (500) exists for review quality |
| Generate stub docs via `doxygen -w` | Mechanical, zero effort | Produces placeholder-only stubs with no semantic content | Content quality requirement |
| Do nothing | No effort | New GPU/SIMD contributors must read both header and implementation to understand preconditions | Ongoing maintenance cost |

## Consequences

- **Positive**: Maintainers and agents can understand preconditions, ownership
  semantics, and error codes from the header alone for the 10 most-used
  internal APIs. IDE hover-docs populate automatically.
- **Negative**: Remaining ~178 headers are still undocumented; follow-up
  rounds are needed.
- **Neutral / follow-ups**: No logic change; no binary or ABI impact. Further
  rounds should cover `feature/feature_extractor.h`, `feature/feature_collector.h`,
  and the DNN headers next.

## References

- ADR-0100: project-wide doc-substance rule
- ADR-0108: six deep-dive deliverables per fork-local PR
- See [ADR-0535](0535-adr-atomic-allocator.md) for the allocator that reserved this number.
- See [ADR-0628](0628-adr-allocator-remote-aware.md) for the remote-aware allocator extension.
- Source: round-4 doxygen-private-headers agent task
