- **`scripts/ci/AGENTS.md` is split into four area indexes.** The generated index sat at 15,982 of
  16,000 bytes. `scripts/docs/agents_index.py` now supports areas: `AGENTS.d/_area-<slug>.md` and an
  `area:` key on each page. `AGENTS.md` lists the areas and `AGENTS-<slug>.md` holds the pages of
  each (gates, release, tidy, tests), every file well under the unchanged limit. Other directories
  render as before. `docs/development/agents-index.md` describes it.
