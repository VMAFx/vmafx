<!-- markdownlint-disable MD013 MD038 MD060 -->
# ADR-0913: Changelog fragment renderer — splice contract is `^## \[`, not `^## `

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `release`, `docs`, `tooling`

## Context

`scripts/release/concat-changelog-fragments.sh` (introduced by ADR-0221)
renders the `## [Unreleased]` block of `CHANGELOG.md` from the
per-fragment files under `changelog.d/<section>/`. The splice into
`CHANGELOG.md` is implemented by two awk passes that use a sentinel
regex to detect "end of the Unreleased block, start of the next
release-please-written `## [vX.Y.Z]` section."

The original sentinel was `^## [^[]` — "any h2 that does not begin with
an open square bracket." That regex assumed fragment bodies would
contain no h2 headings. In practice, **84 of the 100+ in-tree
fragments** started with an in-body h2 (either the redundant section
name `## Fixed` or a per-PR descriptive title like
`## Vulkan submit-pool migration`). When the renderer spliced those
fragments into `CHANGELOG.md`, the *next* `--check` saw the first
in-body `## Foo` and treated everything below it as "release history
to preserve." The next `--write` re-rendered the full body but
preserved the now-out-of-band content, **inflating the file by roughly
3 kB per cycle**. PR #332, PR #383, and PR #401 all noted "pre-existing
CHANGELOG drift"; PR #384 / ADR-0892 identified the renderer + section
hygiene angle but deferred the actual sweep.

Net state on master tip `544299fae1`: `CHANGELOG.md` was **59,757
lines**, ~95 % of which was duplicated content from prior `--write`
cycles. The `--check` gate exited zero only because it compared
self-similar duplicates against the in-tree file.

## Decision

The Unreleased-block splice sentinel is `^## \[`, anchored on the open
square bracket that release-please always writes (and that
`## [Unreleased]` itself uses). Fragment bodies may legitimately
contain `## ` or `### ` headers; only the bracketed `## [version]`
form ends the Unreleased block.

Three companion fixes ride alongside:

1. **Renderer-side defense-in-depth**: `emit_fragment()` demotes any
   leading-line `# ` / `## ` in a fragment body to `**bold**`
   pseudo-headers at render time, so a careless author cannot
   re-introduce the splice-contract violation.
2. **Source-side normalisation**: all 102 in-tree fragments had stray
   `## Section` / `### Section` first-line headers stripped (where they
   duplicated the section name the renderer emits itself) or demoted
   from `## ` to `### ` (so source tree matches the splice contract
   directly, without relying on render-time demotion).
3. **Unknown-subdir guard**: `changelog.d/<dir>/` outside the known
   Keep-a-Changelog set (`added/`, `changed/`, `deprecated/`,
   `removed/`, `fixed/`, `security/`) now emits a stderr warning rather
   than being silently skipped (the PR #384 / ADR-0892 motivation).
   The 32 fragments that lived under `changelog.d/perf/` +
   `changelog.d/performance/` were moved to
   `changelog.d/changed/perf-<topic>.md` per the PR #384 convention.

`CHANGELOG.md` regenerated to **15,030 lines** — a drop of ~44,727
lines of duplicated content. The Unreleased block now matches the
fragment tree exactly; the legacy archive
(`changelog.d/_pre_fragment_legacy.md`, 3,118 lines, frozen verbatim)
is preserved at the top of the block.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Anchor sentinel on `^## \[`** (chosen) | Matches the only header shape release-please writes; immune to any fragment-body content shape; idempotent | Requires updating both awk passes + comment | Correct fix at the contract level |
| Strip all `## ` from fragment bodies and keep the `^## [^[]` sentinel | No renderer change | Doesn't actually fix the bug — any *future* fragment with a `## ` regresses it; relies on an enforcement gate the project lacks | Source-side cleanup alone is fragile |
| Quote the Unreleased block with HTML comment fences (`<!-- BEGIN UNRELEASED --> ... <!-- END UNRELEASED -->`) | Sentinel becomes literal, immune to all markdown shapes | Conflicts with release-please's `## [version]` injection convention; would require forking release-please's `release-type: simple` driver | Too invasive for a regex fix |
| Replace bash+awk renderer with a Python tool | Stronger parser, structured warnings | The shell tool is small, fast, already in CI; rewrite is unjustified for one regex bug | Yagni |

## Consequences

- **Positive**: `--write` is idempotent across re-runs (verified). The
  drift class that bit PR #332, PR #383, PR #401, PR #384 is closed
  at the contract level. Future fragment authors can no longer trip
  the bug by adding a `## ` heading.
- **Positive**: stderr warnings on unknown subdirs surface the PR #384
  failure mode (perf/, performance/) for any future author who names a
  wrong directory.
- **Negative**: the 84 fragment edits + 32 file renames touch many
  in-flight branches that authored fragments under the old shape. Each
  rebase will surface conflict markers on the touched fragment file;
  conflicts resolve by `--theirs` on the path content (rendered body
  is regenerated from disk).
- **Neutral / follow-ups**: a `--lint` mode that fails on stray h1/h2
  in fragment bodies is the obvious next step. Deferred — the
  renderer's render-time demoter already prevents the bug from
  recurring; a separate pre-commit gate is incremental polish.

## References

- ADR-0221 — original fragment-renderer system
- ADR-0892 — perf/ + performance/ unknown-subdir audit (in-flight as
  PR #384)
- PR #332, PR #383, PR #401 — all flagged "pre-existing CHANGELOG.md
  drift" without rooting it
- `scripts/release/concat-changelog-fragments.sh` — the renderer
- `docs/research/0913-changelog-renderer-and-drift-2026-05-31.md` —
  audit dossier
- Source: `req` (user briefing 2026-05-31: "Fix the CHANGELOG.md
  renderer bug + 23k-line drift")
