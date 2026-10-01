# Vendored interfig

`upstream/` holds a byte-identical subset of interfig, the figure engine behind the
animated architecture figures on the Hindsight documentation site. Praetor uses it to draw
its documentation figures ([figures guide](../../../../docs/guides/figures.md)). The decision
record is [ADR-0015](../../../../docs/adr/0015-interactive-figures-from-vendored-interfig.md).

## Source and pin

| Field | Value |
| :-- | :-- |
| Repository | <https://github.com/vectorize-io/hindsight> |
| Path | `hindsight-interfig/` |
| Pinned commit | `ccfe85b4851957ac2adf88b4a9ddf9668b2882f1` |
| Last upstream change under the path | `db03d5448ff1290cc6e501aa7f62d5e4a1e979a3` |
| Fetched | 2026-09-27 |

`vendor.json` records the same pin, the include and exclude lists, and the SHA-256 of every
file under `upstream/`.

## License and credit

<!-- REUSE-IgnoreStart -->
interfig is MIT-licensed, Copyright (c) 2025 Vectorize AI, Inc.
<!-- REUSE-IgnoreEnd -->
`upstream/` has no license
file of its own in the upstream tree, so `upstream/LICENSE` is the repository-root
`LICENSE` at the pinned commit, copied verbatim.

- `REUSE.toml` labels `tools/figures/third_party/interfig/upstream/**` as MIT with an override
  annotation placed after the repository-wide `**` table. This file, `vendor.json` and
  everything outside `upstream/` stay EUPL-1.2.
- The committed player in `tools/figures/dist/` carries an
  `interfig (c) 2025 Vectorize AI, Inc. MIT` banner, because the upstream source has no header of
  its own, and `dist/THIRD-PARTY-LICENSES.txt` carries the full license text
  (`tools/figures/bundle.mjs`).
- Every exported SVG carries a credit comment, and the site footer credits Vectorize
  (`copyright` in `mkdocs.yml`).

Credit to Vectorize for the engine does not imply endorsement. No Hindsight figures,
names or branding are vendored or used.

## What is vendored

Included, relative to `hindsight-interfig/`:

- `README.md`, `package.json`;
- `src/index.tsx`, `src/model.ts`, `src/geometry.ts`, `src/svg.ts`,
  `src/node-figures.ts`, and the four `src/*.test.ts` files;
- `scripts/figure-svg.mjs`, `scripts/figure-loader.mjs`.

Excluded:

- `demo/` and `index.html`: the Vite gallery;
- `figures/`: Hindsight product content;
- `scripts/export.mjs`: clip export through Playwright and ffmpeg;
- `.gitignore`, `.prettierrc`, `tsconfig.json`, `vite.config.ts`: upstream tooling.

## Local adaptations

Files under `upstream/` are never edited. Praetor's adaptations live outside it:

| Adaptation | Where |
| :-- | :-- |
| `role="img"`, `<title>`, `<desc>` and a credit comment injected into each exported SVG | `tools/figures/core.mjs` |
| A static SVG variant (`steps: []`) for reduced motion | `tools/figures/core.mjs` |
| Arrow, Home and End keys across the scenario tabs, and a tab-list label | `tools/figures/keyboard.ts` |
| Theme colours mapped to Material for MkDocs variables, and a focus outline | `tools/figures/figures.css` |

Each adaptation is offered upstream (ADR-0015, section 6). A later sync that brings in the
upstream fix retires the local shim.

## Tests

```bash
node --test 'tools/figures/third_party/interfig/upstream/src/*.test.ts'
```

All 13 upstream tests pass at the pin. On Node 26 the run prints
`[DEP0205] DeprecationWarning: module.register() is deprecated`. The warning comes from
upstream's `scripts/figure-svg.mjs`, which `src/figure-svg.test.ts` starts as a child
process. Praetor's build never calls that script: `tools/figures/core.mjs` imports
`toSvg` from `src/svg.ts` directly.

## Updating the pin

`scripts/sync_interfig.py` owns the pin (ADR-0015, section 8):

| Command | What it does |
| :-- | :-- |
| `python3 scripts/sync_interfig.py verify` | Offline, in `make interfig-verify`: `upstream/` matches the `vendor.json` hashes, the include list covers every file, the LICENSE hash is the reviewed `license_sha256`, and the `REUSE.toml` MIT override is the last annotation covering each vendored file |
| `python3 scripts/sync_interfig.py check` | Compares the newest upstream commit under `hindsight-interfig/` with `path_commit`; exits 3 on drift with the compare URL and the update command, 1 on any other failure |
| `python3 scripts/sync_interfig.py update --commit <sha>` | Vendors `<sha>`, a full 40-character commit sha, runs the upstream tests and the figure build, and rebuilds the committed player |

The weekly `interfig-sync.yml` workflow runs `check` and fails with the update command when
upstream has moved. To update the pin:

1. Run the printed command. It works from any directory and needs `node` and `npm` on `PATH`:

   ```bash
   python3 scripts/sync_interfig.py update --commit <sha>
   ```

   It stops before touching the tree if the repository-root `LICENSE` changed, if `src/` or
   `scripts/` (subdirectories included) holds a file that is neither included nor excluded, or
   if the upstream `react` peer range does not admit the `react` version in
   `tools/figures/package.json`. Otherwise it swaps in the new `upstream/` and `vendor.json`
   together, restoring the previous tree if the swap fails, then runs the upstream tests,
   `npm --prefix tools/figures run build`, the locked
   `npm ci --prefix tools/figures --ignore-scripts` and `npm --prefix tools/figures run bundle`,
   which rewrites the committed player in `tools/figures/dist/`. If any of them fails, revert the
   update. `git clean` removes the new upstream files and figure outputs that `git checkout`
   leaves behind:

   ```bash
   git checkout -- tools/figures/third_party/interfig docs/assets/figures tools/figures/dist
   git clean -fd -- tools/figures/third_party/interfig/upstream docs/assets/figures
   ```

2. Add a new upstream file to the include or exclude list in `vendor.json` first, when step 1
   stops on one.
3. Commit the rewritten `upstream/`, `vendor.json`, the regenerated `docs/assets/figures/` and
   the rebuilt `tools/figures/dist/`.
   The command prints the upstream commits under `hindsight-interfig/` since the old pin and
   the compare URL for the pull-request body. The list is best effort: when GitHub rate-limits
   or fails the request, the update still succeeds and prints only the compare URL.
