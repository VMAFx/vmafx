- **The documentation site has its own design.** A stylesheet on top of
  Material for MkDocs (`docs/stylesheets/vmafx.css`) sets a palette taken from
  the project banner for light and dark mode, holds prose to about 65
  characters per line, sets headings at weight 600 to 700 in the full text
  colour, and sets tables and admonitions at 14.5 px and code at 14 px. The
  landing page leads with what VMAFx is, a first command, the newcomer steps
  and a card per backend. Inter and JetBrains Mono are served from the site
  under the SIL Open Font License instead of a font CDN, and
  `make docs-fragments-check` holds them to the hashes in their `vendor.json`.
  The design, its tokens and the measured values are described in
  [Documentation site design](docs/development/docs-site-design.md)
  ([ADR-1508](docs/adr/1508-docs-site-toolchain-and-charts.md)).
