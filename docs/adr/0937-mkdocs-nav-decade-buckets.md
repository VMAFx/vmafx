<!-- markdownlint-disable MD036 MD060 -->
# ADR-0937: mkdocs ADR nav — per-hundred bucket layout + auto by-tag indexes

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: docs, mkdocs, adr, navigation, automation, fork-local

## Context

The fork now ships 631 ADR files under `docs/adr/`. The previous mkdocs
configuration carved out the entire ADR tree from `nav:` (see the comment
block around `validation.nav.omitted_files: info` in `mkdocs.yml`) on
the grounds that enumerating 600+ files would either explode the nav or
churn it on every PR. The fallback was cross-link navigation from
`docs/adr/README.md` — fine for keyboard users, but invisible in the
material-theme left rail and undiscoverable from the rest of the site.

The corresponding `Tags:` field on every ADR (per the `0000-template.md`
schema) was never surfaced anywhere — `grep -l 'Tags:.*cuda'` was the only
way to slice. 442 distinct tags across the corpus carry real intent
(`gpu`, `vulkan`, `places-4`, `bit-exact`, `vmaf-tune`, `tiny-ai`, …) and
deserve a rendered landing page each.

## Decision

Two complementary generator scripts plus a sentinel-bounded splice region
in `mkdocs.yml`:

1. **`scripts/docs/generate-adr-nav.sh`** emits a YAML fragment under
   `- ADRs:` that buckets ADRs into per-hundred collapsible groups
   (`0000-0099 — Foundation, build, golden gate`, …,
   `0700-0799 — VMAFx rebrand, Go/Rust/C++23, k8s`). Bucket labels live
   in a Python dict inside the script (editorial; edit when a bucket's
   theme drifts). Entries within each bucket are sorted ascending by ADR
   number. The script splices its output between
   `# >>> ADR-NAV-GENERATED` / `# <<< ADR-NAV-GENERATED` sentinel comments
   so round-trip edits are deterministic. `--check` mode compares the
   in-tree block against the freshly-rendered one and exits non-zero on
   drift (CI hook).
2. **`scripts/docs/generate-adr-by-tag.sh`** scans every ADR's `Tags:`
   field (both the modern `- **Tags**: ci, cuda` bullet form and the
   legacy bare `Tags: simd, neon` form are accepted), buckets ADRs by
   tag, and rewrites `docs/adr/by-tag/<tag>.md` (one Markdown table per
   tag) plus a top-level `docs/adr/by-tag/index.md` listing every tag
   with its ADR count. Tag values containing whitespace or angle-bracket
   placeholders (the template's `<comma-separated area tags>` example)
   are filtered out. `--check` mode mirrors the nav script.

The bucket boundary is per-hundred rather than per-decade-of-years
because ADR IDs are assigned in commit order and reset against no
calendar — the per-hundred dividers map cleanly to project phases
(0000s = foundation, 0100s = doc substance, 0700s = VMAFx rebrand) that
already line up with the fork's own narrative.

The `ADRs:` top-level nav entry surfaces:

- `Overview` (existing `adr/README.md`)
- `Template` (existing `adr/0000-template.md`)
- One collapsible group per per-hundred bucket
- A `By tag` collapsible group with the index and one entry per tag

`mkdocs build --strict` passes in ~100 s (vs ~28 s baseline) — the extra
time is the 631 enumerated ADRs + 443 by-tag pages being included in the
build rather than carved out.

## Alternatives considered

| Option | Pros | Cons |
|--------|------|------|
| **A. Per-hundred buckets + by-tag (chosen)** | Discoverable in the left rail; lets the existing `Tags:` schema do useful work without retroactive tagging; deterministic round-trip via sentinels; generator scripts have `--check` for CI drift detection. | Slightly slower build (~100 s vs 28 s baseline); 442 tag entries make the `By tag` group long (mitigated by the dedicated index page acting as primary entry point). |
| B. Keep the existing carve-out (no enumerated nav) | Zero churn; fastest build. | ADRs invisible in the left rail; tag corpus never surfaced; defeats the documentation rule's discoverability intent. |
| C. Enumerate flat alphabetically | Simpler generator. | 631 ADRs in a single accordion is unusable; loses the project-phase narrative the per-hundred grouping conveys. |
| D. Per-decade by calendar year | Maps to time. | ADR IDs are not assigned by date — 0001 and 0042 land within weeks; 0700s and 0042 share project areas. ID order is the only stable invariant. |
| E. Surface only top-N tags in nav | Shorter sidebar. | Hides the long tail; the by-tag index already serves the "find a tag" case; arbitrary cutoff would need maintenance. |

## Consequences

**Positive**

- ADRs are discoverable from the material-theme left rail for the first
  time since the corpus grew past ~50 files.
- Tag corpus surfaces as 442 individual landing pages — `grep -l` is no
  longer the only access path.
- Generator scripts compose with the existing
  `scripts/docs/concat-adr-index.sh` (ADR-0221 fragment renderer); CI can
  add `--check` calls to both new scripts to catch drift on every PR.

**Negative**

- `mkdocs build` time grows from ~28 s to ~100 s; acceptable for the
  documentation site build, which runs once per PR and is not on any
  hot path.
- Bucket labels in `generate-adr-nav.sh` are editorial — they will
  drift over time if not refreshed when a bucket's theme shifts (e.g.,
  0900s onward currently inherit `Misc`). The cost is a one-line dict
  edit.
- 442 entries under `By tag` make that collapsible group long; mitigated
  by the dedicated `Overview` link inside the group acting as the primary
  entry point.

**Neutral follow-ups**

- A future PR may add `--check` invocations of both scripts to
  `.github/workflows/docs.yml` so PRs that add an ADR without
  regenerating fail fast.
- Bucket labels for the 0800s and 0900s can be filled in once those
  buckets have enough ADRs to read a theme off.

## References

- mkdocs-material navigation features —
  <https://squidfunk.github.io/mkdocs-material/setup/setting-up-navigation/>
- ADR-0221 — changelog & ADR fragment pattern (the per-ADR index row
  generator that this PR composes with)
- ADR-0028 / ADR-0106 — ADR maintenance rule (frozen body once Accepted)
- ADR-0386 / ADR-0535 / ADR-0628 — ADR numbering / allocator
- Per user direction: project modernization queue item #13 — restructure
  mkdocs nav for 700+ ADRs by decade + auto-generated by-tag indexes
