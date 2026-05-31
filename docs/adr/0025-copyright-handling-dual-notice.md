<!-- markdownlint-disable MD013 MD060 -->
# ADR-0025: Copyright handling preserves Netflix and adds Lusoris/Claude

- **Status**: Superseded by [ADR-0105](0105-copyright-handling-dual-notice.md)
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: license, docs

## Context

The fork adds wholly-new files (SYCL backend, tiny-AI, MCP server, dev tooling) authored by Lusoris and Claude, while upstream Netflix files continue to be maintained by both parties. License notices must reflect actual authorship without erasing Netflix's ownership of the overall project. User quotes: "update to 2016-2026... they still own this" and "the full sycl thing is btw. written by lusoris and claude not netflix lol".

## Decision

We will: preserve Netflix copyright on Netflix-authored files; bump the year range `2016-2020` → `2016-2026` on those files; put `Copyright 2026 Lusoris and Claude (Anthropic)` on wholly-new fork files under the same BSD-3-Clause-Plus-Patent license; use a dual-copyright notice on mixed files.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep Netflix-only headers everywhere | No divergence | Misattributes wholly-new fork code | User explicitly rejected |
| Replace Netflix with Lusoris everywhere | Clean | Erases original ownership — false and legally dubious | Unacceptable |
| Dual policy (chosen) | Accurate per file | Contributors must pick the right header | Matches reality |

This decision was a default — the alternatives misrepresent authorship.

## Consequences

- **Positive**: file headers match actual authorship; license preserved.
- **Negative**: three header templates to apply correctly.
- **Neutral / follow-ups**: CLAUDE.md §12 rule 7 codifies the templates.

## References

- Source: `req` (user: "update to 2016-2026... they still own this" + "the full sycl thing is btw. written by lusoris and claude not netflix lol")
- Related ADRs: ADR-0008
