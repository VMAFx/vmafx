---
paths:
  - scripts/release/concat-changelog-fragments.sh
  - changelog.d/**
invariant: `CHANGELOG.md` rendered from `changelog.d/<section>/*.md`; keep `^## \[` boundary.
---
<!-- markdownlint-disable MD013 MD038 -->
# Changelog fragment renderer (`release/concat-changelog-fragments.sh`)

## Rebase-sensitive invariants

### `release/concat-changelog-fragments.sh` is the source of truth for `CHANGELOG.md`

Per [ADR-0221](../../docs/adr/0221-changelog-adr-fragment-pattern.md),
`CHANGELOG.md` **rendered** from per-PR fragments under
`changelog.d/<section>/*.md`. Script:

- Renders Keep-a-Changelog section ordering (Added → Changed →
  Deprecated → Removed → Fixed → Security).
- Preserves `changelog.d/_pre_fragment_legacy.md` verbatim at top
  (migrated content from pre-fragment Unreleased block).
- Supports `--check` (CI gate) and `--write` (`release-please`
  rewrite) flags.

**On rebase**: do **not** edit `CHANGELOG.md` by hand. Edit
fragment, run script with `--write`. CI lane
`docs-fragments` runs `--check` on every PR, fails on drift.
Renaming script breaks `release-please` config + CI gate
in same instant.

**Splice contract (ADR-0913)**: script anchors end-of-Unreleased
boundary on `^## \[` — bracketed form `release-please` uses for
released sections (`## [vX.Y.Z] - YYYY-MM-DD`) and
`## [Unreleased]` itself uses. **Do not weaken this regex** to
`^## ` or `^## [^[]` — both shapes tried previously, both
failed when fragment bodies contained `## ` headers (23 k+
line drift PR #332 / #383 / #401 / #384 observed). Fragment
bodies may legitimately contain `## ` or `### ` sub-headings;
renderer demotes leading `# ` / `## ` to `**bold**` at render
time as defense-in-depth. Authors should still write **bullets,
not headers** per `changelog.d/README.md`; demoter for
backwards-compat, not contract.

**Fragment hygiene**: every fragment lives under one of six
Keep-a-Changelog section directories (`added/`, `changed/`,
`deprecated/`, `removed/`, `fixed/`, `security/`). Anything else
triggers stderr WARNING, fragments inside skipped.
PR #384 / ADR-0892 catalogued `perf/` + `performance/`
silent-skip failure mode; ADR-0913 added visible WARNING.
Empty fragments also emit stderr WARNING + skip.
