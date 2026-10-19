- **The documentation renders formulas as math
  ([ADR-2705](docs/adr/2705-docs-math-katex.md)).** Write `$...$` inline or
  `$$...$$` as a display block; KaTeX 0.18.9 typesets it from files the site
  serves itself, with no third-party host. The metric, backend, API, usage and
  development pages that wrote formulas as code text now use TeX, and
  `scripts/docs/check_math.py` fails the docs build on a formula KaTeX rejects.
  See [Writing math](docs/development/docs-site-design.md#writing-math).
