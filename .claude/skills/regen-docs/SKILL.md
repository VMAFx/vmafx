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
   - `mkdocs --version` and confirm the `mkdocs-material` theme is on the venv.
   - Bail with install hints if missing (`pip install mkdocs mkdocs-material`).
2. Run `mkdocs build --strict` from the repo root. The site config is `mkdocs.yml`;
   output lands under `build-docs/site/`.
   - `--strict` fails the build on any warning (broken link, missing nav target, etc.).
3. Surface broken cross-refs:
   - mkdocs-material emits `INFO`-level "not found / unrecognized" messages for
     ADR / research / source-tree refs that don't resolve. Capture
     `/tmp/mkdocs_build.log` and grep for `INFO` lines.
   - Categorise: (a) source-tree refs (`../../core/...`) — inherently
     unresolvable from the mkdocs site; expected. (b) doc-to-doc refs —
     these are the actionable drift, mostly ADR slug renames.
4. Validate ADR coherence:
   - Every `docs/adr/NNNN-*.md` has an index row in `docs/adr/README.md`.
   - Every `(adr/NNNN-slug.md)` ref in `docs/state.md` / `docs/rebase-notes.md`
     points at the actual on-disk filename for that NNNN.
5. Diff the generated site against the previous run (`git diff --no-index` on
   `build-docs/site/`); flag suspicious deletions.
6. If `--open`: open `build-docs/site/index.html` in the user's browser.
7. Print a summary: build status, broken-link count (split by category), pages
   added/removed.

## Guardrails

- `--strict` fails the skill on any mkdocs warning.
- Never edits doc sources — only regenerates output and surfaces drift. If
  ADR slugs have drifted (concept-stable but filename evolved), open a separate
  scoped repair PR — don't bulk-rewrite as a side effect.
- The 5-point ADR-0042 tiny-AI doc bar and the per-surface ADR-0100 bars
  are independent of regen-docs — this skill verifies *coherence*, not
  *substance*.

## Notes

- The legacy Doxygen + Sphinx setup was retired around 2026-04-30 in favour
  of mkdocs-material. `Doxyfile.in` at `core/doc/Doxyfile.in` survives but
  is not wired into a build target.
- mkdocs `INFO` messages are **not** promoted to warnings under `--strict`
  by default. To make doc-to-doc drift fatal, set
  `validation.unrecognized_links: warn` in `mkdocs.yml`.
