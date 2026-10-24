<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3366: One README presentation standard, with VMAFx as the reference example

- **Status**: Proposed
- **Date**: 2026-10-22
- **Deciders**: Lusoris
- **Tags**: `docs`, `process`

## Context

The VMAFx README showed its banner at the full column width, so the banner
filled the first screen. About 25 badges sat in a "Project status" section in
the middle of the page, mixing CI status with language and backend versions,
and a second section of the same name followed further down. The backend
dispatch diagram was a static SVG exported for the documentation site: on
GitHub's column, about 830 pixels wide, it was scaled down to washed-out,
hard-to-read text. The other ecosystem repositories have each drifted their
own way, and nothing states what a README should look like.

## Decision

We define one README presentation standard in
[docs/README-STANDARD.md](../README-STANDARD.md) and apply it to the VMAFx
README as the reference example. The standard fixes a five-colour palette, a
banner at most 120 pixels tall, one `for-the-badge` badge block under the
banner in the order release, CI, security, standards, licence, support,
compact version rows inside the sections they describe, Mermaid diagrams
themed with the palette through the `base` theme and `classDef`, and one
section order with each heading used once. Applying the standard to the other
repositories and a shared CI check that keeps READMEs on it follow after the
operator approves this example; they are not part of this decision's change.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Mermaid themed with the palette (chosen) | GitHub draws it at the column width; text stays sharp; colours set in the source; no build step | Only the `base` theme can be changed; layout is Mermaid's | Chosen |
| Keep the exported static SVG, resized | Same figure as the documentation site | Still a second copy to keep in step; text size depends on scaling; no theme switch | The figure pipeline targets the site's wider pages; resizing does not fix legibility on a phone |
| Badges kept in a "Project status" section | Header stays short | Status is the first thing a visitor looks for; mixed with version badges it is hard to read | Moves status out of sight; the operator asked for badges at the top |
| One badge row with every version badge too | Everything in one place | About 25 large badges push the content off the first screen | Version badges move to a compact row in their section |
| Per-repository styles | No coordination | Every repository looks different; nothing to check against | A shared check needs one standard |

## Consequences

- **Positive**: the first screen shows what the project is and whether its
  builds are green; the diagram is readable on GitHub's light and dark themes;
  reviewers have a written standard to check READMEs against.
- **Negative**: the README diagram and the site figure are drawn separately;
  a change to backend dispatch updates both.
- **Neutral / follow-ups**: after approval, the standard is applied to every
  public ecosystem repository and the four organisation profiles, and a shared
  CI check enforces the badge order, the banner height and the absence of
  full-width banners.

## References

- Operator order recorded in the controller ledger as
  `readme-standard-decided-20261022` and `vmafx-readme-standard-exemplar-20261022`
  (paraphrased): one ecosystem README presentation standard with grouped
  badges at the top, a small banner, coloured diagrams that fit GitHub's
  column, one section order; VMAFx first as the example, the sweep after
  approval.
- shields.io static badge and style parameters: <https://shields.io/badges>.
- Mermaid theming (`base` theme, `themeVariables`, `classDef`):
  <https://mermaid.js.org/config/theming.html>.
