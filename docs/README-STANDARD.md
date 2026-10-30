# README presentation standard

This page defines how the front page (`README.md`) of a VMAFx ecosystem
repository looks: what sits at the top, which badges appear and in which
order, how large the banner is, how diagrams are drawn and in which order the
sections follow. The VMAFx repository's own [README](../README.md) is the
reference example. The decision record is
[ADR-3366](adr/3366-readme-presentation-standard.md).

GitHub renders a README in a column about 830 pixels wide on a desktop
browser, and narrower on a phone. Every rule below is written for that column.

## Quick checklist

Use this list when you write or review a README:

1. Title area: a banner at most 120 pixels tall, never `width="100%"`.
2. Directly under the banner: one badge block in the fixed group order below,
   with every badge served fast enough for GitHub's image proxy (see "Badge
   sources"). No badges anywhere else at that size.
3. Under the badges: a one-line tagline in bold, then at most one short
   paragraph that says what the project is.
4. Diagrams: Mermaid, themed with the palette below; a sized SVG only where
   Mermaid cannot draw the figure.
5. Sections in the order given below, each heading used once.

## Palette

Badges, the banner and diagrams use the same five colours. They are taken from
the VMAFx banner and documentation site.

| Token | Hex | Use |
| --- | --- | --- |
| `ink` | `#0b1020` | banner and diagram background, text on cyan fills |
| `panel` | `#111a35` | badge label background (`labelColor`), diagram node fill |
| `cyan` | `#06b6d4` | first accent: inputs, GPU paths, standards badges |
| `blue` | `#3b82f6` | second accent: edges, borders, release and licence badges |
| `violet` | `#8b5cf6` | third accent: decisions, CPU paths, support badges |
| `text` | `#e2e8f0` | light text on dark fills |
| `muted` | `#94a3b8` | secondary text in the banner |

The static badge messages use the three accent colours and set
`labelColor=111a35` so the block reads as one band. Workflow-status badges are
GitHub's own and keep its green, red or grey colour, because that colour is the
status.

## Hero banner

- One SVG file in the repository, drawn on a 1200 x 120 view box, so it scales
  without blurring.
- Shown with `height="120"` (or less) inside the centred header block; never
  `width="100%"`, which makes the banner fill the whole column and pushes the
  badges below the first screen.
- Contents: the wordmark, and at most two short lines of muted text. The
  description belongs in the tagline, where it can be read and searched.
- An `alt` text that says what the project is.

## Badge block

All badges at the top sit in one block directly under the banner, inside the
centred header; static badges use `style=for-the-badge`. The groups always
come in this order; a repository leaves out a group it has nothing for, but
never reorders them:

1. **Release / version**: the current release as a static badge that
   release-please keeps up to date (see "Badge sources").
2. **CI / build**: one workflow-status badge per main workflow, with
   `branch=<default>&event=push`. The badge label is the workflow name, so a
   red badge points at the workflow of the same name.
3. **Security / scorecard**: the security workflow, the OpenSSF Scorecard and
   the OpenSSF Best Practices level, where the project has them.
4. **Standards**: the engineering standards the project declares (for VMAFx:
   HISS-21, NASA/JPL Power of 10, Conventional Commits).
5. **Licence**: one badge naming every licence that applies.
6. **Support**: sponsorship links (GitHub Sponsors, Ko-fi and the like).

Each group is introduced by an HTML comment (`<!-- 2 CI/build -->`) so a
reviewer can check the order without rendering the page.

Version badges for languages, toolchains, backends and integrations (C, Go,
Rust, Python, CUDA, ROCm and the like) are not part of the top block. Where
they help, they go in one compact row (`style=flat-square`) inside the section
they describe; for VMAFx that is "Backends at a glance". No `for-the-badge`
badge appears below the header.

A static badge is built from shields.io's path form,
`https://img.shields.io/badge/<label>-<message>-<colour>`, where `-` inside a
label is written `--`, `_` becomes a space and `__` an underscore. The
parameters this standard uses are `style`, `labelColor`, `logo` (a Simple
Icons name) and `logoColor`.

### Badge sources

GitHub loads every README image through its image proxy (camo), which gives up
after about four seconds and then shows a broken image. Badges that shields.io
computes on request (release, workflow status, Scorecard) often take one to
five seconds when shields.io has no cached copy, so on github.com they broke on
most page loads. Every badge in the top block therefore comes from a source
that answers well inside that limit:

| Badge | Source |
| --- | --- |
| Workflow status | GitHub's own `https://github.com/<owner>/<repo>/actions/workflows/<file>/badge.svg?branch=<default>&event=push`; GitHub serves it directly, not through the proxy. |
| Release | A static shields.io badge in query form, `https://img.shields.io/static/v1?label=release&message=v<version>&...`, on a line that ends with `<!-- x-release-please-version -->`. `README.md` is a generic extra file in `release-please-config.json`, so each release PR rewrites the version, and `scripts/release/verify-release-version.sh` checks the marker against the tag. |
| OpenSSF Best Practices | `https://www.bestpractices.dev/projects/<id>/badge`, served by the project itself. |
| OpenSSF Scorecard | A static link badge to the Scorecard viewer. The score badge (`api.scorecard.dev/.../badge`) redirects to a computed shields.io badge and is too slow for the proxy. |

Measure a new badge source before adding it: `curl -o /dev/null -w '%{time_total}'`
on an uncached URL must stay well under four seconds.

## Tagline and introduction

- One bold line under the badges that says what the project does.
- Optionally one italic line of context (for VMAFx: that it is a Netflix/vmaf
  fork that keeps the reference scores).
- Then, outside the centred block, one short paragraph and the documentation
  link.

## Diagrams

Diagrams in a README are Mermaid code blocks, which GitHub draws in the page
at the column's width and in its own colours, so they stay sharp and readable.
A static SVG exported from another tool is drawn at its own size: it gets
scaled down until the text is too small to read, or clipped.

Every Mermaid diagram starts with the same init line, which selects the only
theme Mermaid lets you change (`base`) and sets the palette:

```text
%%{init: {'theme': 'base', 'themeVariables': {'fontFamily': 'ui-sans-serif, Segoe UI, Helvetica, Arial, sans-serif', 'fontSize': '15px', 'primaryColor': '#111a35', 'primaryTextColor': '#e2e8f0', 'primaryBorderColor': '#3b82f6', 'lineColor': '#3b82f6', 'edgeLabelBackground': '#111a35', 'tertiaryColor': '#0b1020'}}}%%
```

Node roles are then coloured with `classDef` and `:::class`:

```text
classDef input fill:#0b1020,stroke:#06b6d4,stroke-width:2px,color:#e2e8f0
classDef step fill:#111a35,stroke:#3b82f6,stroke-width:2px,color:#e2e8f0
classDef decide fill:#111a35,stroke:#8b5cf6,stroke-width:2px,color:#e2e8f0
classDef gpu fill:#06b6d4,stroke:#0b1020,stroke-width:2px,color:#0b1020
classDef cpu fill:#8b5cf6,stroke:#0b1020,stroke-width:2px,color:#f8fafc
```

Rules:

- Top-to-bottom (`flowchart TD`) for anything with more than four nodes in a
  chain; a left-to-right chain is squeezed to fit the column.
- Node text at most about 35 characters per line; break longer text with
  `<br/>`.
- Every node fill is dark with light text or an accent with dark text, so the
  diagram reads on GitHub's light and dark themes alike.
- Where Mermaid cannot draw the figure, use an SVG drawn for the column: a
  view box no wider than 830 units, text at least 14 units, palette colours,
  and a dark background of its own.

The documentation site keeps its own figure pipeline (`docs/figures/`,
[documentation site design](development/docs-site-design.md#diagrams)); this
standard governs the README only.

## Section order

Sections follow this order. A repository leaves out what it has nothing for;
each heading appears once.

1. Header: banner, badge block, tagline (centred HTML block).
2. Introduction paragraph and documentation link.
3. Why this project (what it adds, compared with the alternatives).
4. Get started (install, first command).
5. Architecture or "at a glance" (the diagram and the compact version row).
6. Guides and reference.
7. Contribute.
8. Support.
9. Roadmap and releases.
10. Upstream and licence.
11. Standards and governance (managed blocks, such as praetor's governance
    block, stay last and are never edited by hand).

## Checking a README

Look at the rendered page on the forge, not the Markdown source: open the
README on its branch in a browser window about 830 pixels wide and check that

- the banner is short and the badges are visible without scrolling on a
  desktop screen,
- the badge groups come in the order above,
- every diagram is drawn in colour, readable, and not cut off at the right.

A passing Markdown lint or CI run does not show any of these.
