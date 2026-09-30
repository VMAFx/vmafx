# Interactive figures

This directory holds the figure engine for this repository's documentation. A figure is a
TypeScript spec under `docs/figures/`. The engine renders it into committed SVG and JSON files
under `docs/assets/figures/`. On an MkDocs or Astro Starlight site each figure plays its scenario
tabs with moving packets, narration, pause and speed control. A README, a wiki page or a browser
without JavaScript shows the same figure as an SVG with a caption and a text description.

`praetorctl adopt` writes the files in this directory. `praetorctl audit` fails when one of them
differs from the copy the binary carries, and `praetorctl adopt --force` restores it. Edit the
specs and your site configuration, not these files.

## Requirements

- Node.js 22.18 or later renders and checks figures. No npm install is needed.
- An MkDocs site build needs only Python and MkDocs: `mkdocs_hook.py` uses the Python standard
  library and the MkDocs API.
- An Astro Starlight site needs no extra package: `astro.mjs` imports only Node built-ins and the
  engine's own modules.

## Add a figure

1. Read the code the figure will show. Every box, edge and narration line should match the code
   the spec cites as evidence.
2. Write the spec as `docs/figures/<slug>.ts`, with a lowercase kebab-case slug:

   ```ts
   import type { PraetorFigure } from '../../tools/figures/types.ts';

   export default {
     title: 'Request path',
     alt: 'The router hands each request to its handler, which reads the store.',
     evidence: ['src/router.ts:route', 'src/router.ts:handle'],
     props: {
       layout: {
         direction: 'row',
         children: [
           { id: 'router', label: 'Router' },
           { id: 'handler', label: 'Handler' },
           { id: 'store', label: 'Store', shape: 'store' },
         ],
       },
       edges: [
         { from: 'router', to: 'handler' },
         { from: 'handler', to: 'store', label: 'read' },
       ],
       steps: [
         {
           label: 'Read',
           caption: 'One read, from the router to the store.',
           flow: [
             { edges: 'router->handler', say: 'The router picks the handler.' },
             { edges: 'handler->store', say: 'The handler reads the store.' },
           ],
         },
       ],
     },
   } satisfies PraetorFigure;
   ```

3. Render the outputs:

   ```bash
   node tools/figures/build.mjs build
   ```

   For each spec this writes `docs/assets/figures/<slug>.svg` (animated),
   `<slug>.static.svg` (shown under `prefers-reduced-motion`) and `<slug>.json` (the caption,
   the text description, the hashes the checks compare and the figure markup). It also deletes
   the outputs of a spec that no longer exists.
4. Name the figure on a page with a `figure` fence that holds only the slug:

   ````markdown
   ```figure
   request-path
   ```
   ````

5. Run the checks in [Checks](#checks), then commit the spec and its three outputs.

## Spec rules

`types.ts` declares the spec format, and `validate` in `core.mjs` enforces it when a figure is
built or checked, so a spec that skips the type checker still fails:

- `alt` is one sentence of at most 125 characters. `title` becomes the caption.
- `evidence` lists at least one `path:Symbol` anchor. `build.mjs sources` fails when the path is
  gone or the symbol no longer occurs in the file.
- Every label, caption, narration line and card row is a string, so the player, the SVG and the
  text description show the same content.
- Box and group ids are unique. Every edge end names a box or a group, and every beat names an
  existing edge. An edge's id defaults to `from->to`.
- `shape: 'store'` draws data at rest and `shape: 'decision'` a decision.
- `describe` adds lines to the text description derived from the spec.
- A spec stays within `LIMITS` in `core.mjs`: 40 boxes, 40 groups, 80 edges and 12 steps, with
  at most 32 beats per step.

## Show figures on a site

**MkDocs.** List the hook and keep the specs out of the published pages in `mkdocs.yml`:

```yaml
hooks:
  - tools/figures/mkdocs_hook.py
exclude_docs: |
  /figures/
```

The hook replaces each `figure` fence with the figure markup and publishes `figures.css` and the
player files in `dist/`, so the site needs no `extra_css` or `extra_javascript` entry. A fence that
names a figure without a JSON file logs a warning, which fails `mkdocs build --strict`.

**Astro Starlight.** Add the integration and the stylesheet to `astro.config.mjs`:

```js
import figures from './tools/figures/astro.mjs';

export default defineConfig({
  integrations: [starlight({ customCss: ['./tools/figures/figures.css'] }), figures()],
});
```

The integration renders each `figure` code block in `.md` and `.mdx` pages, loads the player on
every page, and serves and copies the figure and player files. A block that names a figure without
a JSON file fails the build.

## Outside the site

- **README.** Put a figure between `<!-- figure:<slug> -->` and `<!-- /figure -->`, then fill the
  block with repository-relative image paths:

  ```bash
  node tools/figures/build.mjs portable --write README.md
  ```

- **Wiki pages.** `node tools/figures/build.mjs portable --base <url> <page>...` replaces each
  `figure` fence with absolute image URLs under `<url>` and a link to the interactive figure.
  `--wiki` takes the URL from `site_url` in `mkdocs.yml` instead.

## Checks

| Command | Fails when |
| :-- | :-- |
| `node tools/figures/build.mjs check` | a spec breaks a rule, or a committed output differs from a fresh render |
| `node tools/figures/build.mjs sources` | a JSON no longer matches its spec, the engine or its SVGs; a spec or JSON lacks its pair; a fence names an unknown figure; the README block is stale; an evidence anchor is gone |
| `node tools/figures/build.mjs site --config mkdocs.yml --docs docs --site site` | after the site build: a fence did not become a figure, an image does not resolve, or a page does not load the player |

`make docs-figures` runs `check` and `sources`. `praetorctl adopt` attaches that target to
`verify-all`, and the documentation workflow, `.github/workflows/praetor-docs.yml`, runs both
commands after its Markdown check. In a repository with no spec and no committed output, both
commands print that they skipped and why, and exit 0.

`sources` reads the pages of the site configuration at the repository root: `mkdocs.yml` (or
`mkdocs.yaml`) with the pages under `docs/`, and `astro.config.mjs` (or another
`astro.config.*` name Astro loads) with the pages under `src/content/docs/`. With both, it reads
both; its success line names the pages it read. `--config` names another configuration and
`--docs` its pages directory. For a Starlight build, pass
`--config astro.config.mjs --docs src/content/docs --site dist` to `site`, and the site's base
path as `--base`. Each command exits 0 on a pass, 1 on findings and 2 on a usage error or an
input it cannot read. `node tools/figures/build.mjs` without a command prints every command and
option.

**First run.** A spec has no outputs until `node tools/figures/build.mjs build` renders them,
and a site build fails on a `figure` fence whose JSON is missing. After you add a spec, or copy
in a documentation preset that ships one without its outputs, run `build` before the first site
build and commit the outputs. Until then `check` and `sources` fail and name that command.

The checks compare hashes of the engine, the specs and the outputs, so `praetorctl adopt` also
writes a managed block at the end of `.gitattributes` that keeps those files at LF on every
platform and the vendored interfig files byte for byte.

## Files

| Path | Role |
| :-- | :-- |
| `core.mjs` | Validates a spec and renders its SVGs, text description and markup. |
| `build.mjs`, `checks.mjs` | The command line and the `sources`, `site` and `portable` checks. |
| `types.ts` | The spec type a `docs/figures/<slug>.ts` file checks against. |
| `third_party/interfig/` | The vendored interfig render source, byte-identical to its pin in `vendor.json`, with its upstream `LICENSE`. |
| `dist/` | The player: `loader.js` runs on every page and loads `player.js` when a figure nears the viewport; `THIRD-PARTY-LICENSES.txt` holds the license texts of the code both files bundle. |
| `figures.css` | The figure stylesheet, following the site's light and dark palette. |
| `mkdocs_hook.py`, `astro.mjs`, `serve.mjs` | The MkDocs hook, the Astro integration and the file server it uses. |

An edit to `core.mjs` or to the vendored render files changes the engine hash recorded in every
figure's JSON, so after an update that changes them, run `build.mjs build` and commit the new
outputs.

## Credit

The engine is interfig by Vectorize AI, Inc., under the MIT License
(`third_party/interfig/upstream/LICENSE`). The player bundles interfig with React, react-dom and
scheduler, whose license texts are in `dist/THIRD-PARTY-LICENSES.txt`. Every exported SVG and both
player scripts carry the interfig credit; keep these notices in every copy. A repository that
declares its licensing in `REUSE.toml` labels `tools/figures/third_party/interfig/upstream/**` MIT
in an override annotation placed after any table that covers the whole tree; `praetorctl audit`
warns while that override is missing.
