- Added the credits page `docs/credits.md`, rendered from the curated list
  `docs/credits.yaml`: every third-party project, vendored file, model, dataset,
  paper, tool, action, image and font VMAFx ships, adapts or uses, with its
  relation to the project and the licence its upstream states. `make
  docs-fragments-check` fails on page drift, an uncredited vendored or
  inherited path, an unused `LICENSES/*.txt`, a skill derived from an upstream
  with no entry, and an entry path that is gone. See ADR-2485.
