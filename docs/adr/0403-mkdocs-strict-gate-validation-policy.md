<!-- markdownlint-disable MD060 -->
# ADR-0403: mkdocs `--strict` validation policy — actionable carve-outs

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: docs, ci, mkdocs, fork-local

## Context

`.github/workflows/docs.yml` has run `mkdocs build --strict` since the lane
landed, but `mkdocs.yml` set every link-validation category to `info`
(`links.not_found: info`, `links.anchors: info`,
`links.unrecognized_links: info`). `--strict` only fails on `WARNING`
or higher, so the lane was effectively a smoke test that the YAML
parsed and the theme rendered. After ~480 PRs of doc accretion the
fork's docs tree had picked up two genuinely-broken in-doc anchors
(`mcp/embedded.md` → ADR-0209, `research/0055` → Research-0053) and
50 leaked fragment-tree pages (`adr/_index_fragments/**`,
concatenation source per ADR-0221, never intended as standalone
pages). The lane caught none of it.

The naive fix — promote every category to `warn` — emits 1,276
WARNINGs against current `master`. Two populations dominate and
neither is fixable under existing fork policy:

1. **Cross-tree pointers** (~820 warnings): docs link to source-tree
   files / dirs that sit outside `docs_dir` (`../../core/src/...`,
   `../../scripts/ci/...`, `../../.github/workflows/...`). They render
   fine on GitHub's web view (which is where contributors most often
   read these files), but mkdocs cannot resolve them because they're
   outside the rendered site. Removing them strips real navigation
   value; converting all of them to absolute GitHub URLs is a 800+
   touch sweep across primarily-immutable ADR bodies.
2. **ADR-body cross-refs to renamed neighbours** (~360 warnings):
   ADRs cite each other by slug; when an ADR's slug was tightened
   (e.g., `0138-simd-bit-exactness-policy.md` → `0138-iqa-convolve-
   avx2-bitexact-double.md`) the citing ADR's body still points at
   the old slug. ADR-0028 / ADR-0106 freeze ADR bodies once `Status:
   Accepted`. The citing side cannot be edited.

## Decision

Tighten the `validation:` block in `mkdocs.yml` so `--strict` fails
on actionable categories while documenting carve-outs for the two
unfixable populations:

- `links.anchors: warn` — actionable (in-doc heading anchors).
- `nav.not_found: warn` — actionable (typos in `mkdocs.yml` `nav:`).
- `nav.omitted_files: info` — the fork deliberately keeps 260+ ADRs
  and 80+ research digests off the explicit nav (they navigate via
  cross-link from `adr/README.md` / `research/README.md`).
- `links.not_found: info` — carve-out for the cross-tree-pointer
  and renamed-ADR populations described above.
- `links.unrecognized_links: info` — same population shape (trailing-
  slash dir refs into the source tree).

Exclude `docs/adr/_index_fragments/**` from the rendered site via
`exclude_docs:`; they are concatenation source per ADR-0221.

Sweep the actionable subset on this PR: fix the two anchor warnings,
fix the bare-relative-dir links in `docs/index.md`, `docs/state.md`,
and `docs/rebase-notes.md`. Do not touch ADR bodies.

The keep-at-info decisions are revisitable: if a future sync moves
the cross-tree pointers into `docs/`-resident content (e.g., generated
API stubs under `docs/api/_generated/`), or if ADR-0028 is superseded,
flip those categories to `warn` and clear the residual.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Promote every category to `warn` | Maximally strict | 1,276 unfixable WARNINGs against `master`; lane is permanently red until ADR-0028 is repealed | Blocks all PRs |
| Keep all categories at `info` (status quo) | Zero churn | `--strict` flag is decorative; future broken anchors / fragment leaks land silently | Defeats the gate |
| Rewrite all cross-tree pointers to absolute GitHub URLs | Categorically eliminates the population | ~820 ADR-body edits; collides with ADR-0028 immutability; loses GitHub's "click the file" relative-link rendering | Out of scope |
| Drop `--strict` flag from CI | Simplest | No gate at all; regresses to "docs CI checks YAML parses" | Defeats the gate |
| Carve out the two unfixable populations + sweep actionable subset (chosen) | Catches new orphan-anchor and fragment-leak regressions; clears the actionable backlog; preserves cross-tree pointer convention | Adds explanation block to `mkdocs.yml`; requires future readers to understand why two categories are at `info` | Best fit for the fork's existing constraints |

## Consequences

- **Positive**: docs-CI lane now actively fails on broken in-doc
  anchors, mkdocs nav typos, and leaked excluded-tree pages. Anchor
  regressions during ADR / research / topic-tree edits get caught
  on PR rather than landing on `master`.
- **Negative**: residual 1,180+ INFO-level link issues remain in
  the build log; readers parsing `--strict` output must skim past
  them to find the actionable WARNINGs. Mitigated by the carve-out
  comment block in `mkdocs.yml` explaining the policy in-line.
- **Neutral / follow-ups**: tracked at `info` so future PRs that
  happen to fix one of these (e.g., when an ADR is superseded and
  the citing ADR rewritten) reduce the INFO count opportunistically.
  When ADR-0028 is superseded by a less-strict immutability rule,
  re-evaluate flipping `links.not_found` to `warn`.

## References

- ADR-0028, ADR-0106 — ADR-body immutability rule.
- ADR-0221 — `docs/adr/_index_fragments/**` concatenation pattern.
- `.github/workflows/docs.yml` — the strict gate that this ADR
  parameterises.
- Source: implementation task — "tighten the docs build to
  `mkdocs build --strict`".
