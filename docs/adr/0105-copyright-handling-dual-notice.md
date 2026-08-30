<!-- markdownlint-disable MD060 -->
# ADR-0105: Copyright handling preserves Netflix and adds Lusoris/Claude

- **Status**: Accepted
- **Supersedes**: [ADR-0025](0025-copyright-handling-dual-notice.md)
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: license, docs

## Context

The fork adds wholly-new files (SYCL backend, tiny-AI, MCP server, dev
tooling) authored by Lusoris and Claude, while upstream Netflix files
continue to be maintained by both parties. License notices must reflect
actual authorship without erasing Netflix's ownership of the overall
project. The user directed two clarifications: bump the year range on
existing Netflix files (so they currently read `2016–2026`, since
Netflix continues to own them) and recognise that wholly-new fork
subtrees — notably the SYCL implementation — are authored by Lusoris
and Claude rather than Netflix.

This ADR re-states ADR-0025's decision in neutral English so the body
no longer carries colloquial verbatim quotes. The decision itself is
unchanged; only the prose register is.

## Decision

Preserve Netflix copyright on Netflix-authored files; bump the year
range `2016–2020 → 2016–2026` on those files; place
`Copyright 2026 Lusoris and Claude (Anthropic)` on wholly-new fork files
under the same BSD-3-Clause-Plus-Patent license; use a dual-copyright
notice on mixed files (e.g. fork-modified Netflix sources).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep Netflix-only headers everywhere | Zero divergence from upstream | Misattributes wholly-new fork code authored entirely by Lusoris/Claude | Rejected per user direction |
| Replace Netflix with Lusoris everywhere | Single template | Erases original ownership — factually wrong and legally indefensible | Unacceptable |
| Dual policy per file (chosen) | Notice matches actual authorship per file | Contributors must select the correct header template | Matches reality |

The narrow alternatives misrepresent authorship; the dual-policy choice
was effectively a default once the misrepresentation was ruled out.

## Consequences

- **Positive**: file headers match actual authorship; Netflix license
  preserved; the SYCL subtree carries accurate Lusoris/Claude attribution.
- **Negative**: three header templates to keep straight; new files
  require selecting the right one.
- **Neutral / follow-ups**: [CLAUDE.md §12 rule 7](../../CLAUDE.md)
  codifies the templates; ADR-0025 remains in the tree, marked
  `Superseded by ADR-0105`.

## References

- Source: `req` (user: "update to 2016-2026... they still own this" +
  "the full sycl thing is btw. written by lusoris and claude not netflix lol")
- Supersedes: [ADR-0025](0025-copyright-handling-dual-notice.md)
- Related ADRs: [ADR-0008](0008-readme-fork-rebrand.md)
