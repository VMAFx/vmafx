---
name: regen-docs
description: Regenerate the mkdocs-material site, validate cross-references, surface stale or broken links.
---
<!-- markdownlint-disable MD013 -->

# /regen-docs

## Invocation

```text
/regen-docs [--strict] [--open]
```

## Steps

1. Verify tools:
   - `mkdocs --version`, confirm `mkdocs-material` theme on venv.
   - Bail with install hints if missing (`pip install mkdocs mkdocs-material`).
2. Run `mkdocs build --strict` from repo root. Site config = `mkdocs.yml`;
   output lands under `build-docs/site/`.
   - `--strict` fails build on any warning (broken link, missing nav target,
     etc.).
3. Surface broken cross-refs:
   - mkdocs-material emits `INFO`-level "not found / unrecognized" messages
     for ADR / research / source-tree refs that don't resolve. Capture
     `/tmp/mkdocs_build.log`, grep for `INFO` lines.
   - Categorise: (a) source-tree refs (`../../core/...`) — inherently
     unresolvable from mkdocs site; expected. (b) doc-to-doc refs — actionable
     drift, mostly ADR slug renames.
4. Validate ADR coherence:
   - Every `docs/adr/NNNN-*.md` has index row in `docs/adr/README.md`.
   - Every `(adr/NNNN-slug.md)` ref in `docs/state.md` / `docs/rebase-notes.md`
     points at actual on-disk filename for that NNNN.
5. Diff generated site against previous run (`git diff --no-index` on
   `build-docs/site/`); flag suspicious deletions.
6. If `--open`: open `build-docs/site/index.html` in user browser.
7. Print summary: build status, broken-link count (split by category), pages
   added/removed.

## Guardrails

- `--strict` fails skill on any mkdocs warning.
- Never edits doc sources — only regenerates output, surfaces drift. If
  ADR slugs drifted (concept-stable but filename evolved), open separate
  scoped repair PR — don't bulk-rewrite as side effect.
- 5-point ADR-0042 tiny-AI doc bar and per-surface ADR-0100 bars independent
  of regen-docs — skill verifies *coherence*, not *substance*.

## Notes

- Legacy Doxygen + Sphinx setup retired around 2026-04-30 in favour of
  mkdocs-material. `Doxyfile.in` at `core/doc/Doxyfile.in` survives but not
  wired into build target.
- mkdocs `INFO` messages **not** promoted to warnings under `--strict` by
  default. To make doc-to-doc drift fatal, set
  `validation.unrecognized_links: warn` in `mkdocs.yml`.
