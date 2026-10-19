<!-- markdownlint-disable MD013 MD060 -->
# ADR-2705: formulas are TeX rendered by self-hosted KaTeX, checked at build time

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: maintainer, agent
- **Tags**: docs, build, supply-chain, fork-local

## Context

The documentation wrote its formulas as code text: a `text` block such as
`psnr_p = 10 * log10(peak^2 / mse_p)` or an inline code span such as
`ceil(10 * log10(peak^2 / (min_sse / n_samples)))`. Those read as source code,
lose the fraction, root, sum and subscript structure, and cannot be told apart
from a C expression that the page quotes literally. The site (Material for
MkDocs) had no math support. The site already serves its fonts and its chart
runtime itself, with a `vendor.json` of hashes per vendored directory
([ADR-1508](1508-docs-site-toolchain-and-charts.md)), and loads nothing from
another host. A formula renderer has to keep that property, and a formula that
does not compile has to fail the strict documentation build instead of showing
as red text on the published site.

## Decision

Formulas are TeX between dollar signs: `$...$` inline and `$$...$$` display,
read by `pymdownx.arithmatex` in generic mode, which keeps the Markdown valid
for GitHub's own renderer. KaTeX 0.18.9 renders them in the browser from files
the site serves itself (`docs/javascripts/vendor/katex/`): `katex.min.js`,
`auto-render.min.js`, `katex.min.css` and the WOFF2 fonts, from the npm
tarball pinned by exact version, SHA-256 and npm SHA-512 integrity in
`vendor.json` and written by `scripts/docs/vendor_katex.py`, held to the
recorded hashes by the existing `scripts/docs/check_vendored_assets.py`.
`docs/javascripts/katex.js` typesets only the elements `arithmatex` writes and
only its `\(` and `\[` delimiters, so a stray dollar sign in prose is never
read as math.

`scripts/docs/check_math.py` compiles every formula of the built site with the
vendored KaTeX in strict mode with `throwOnError`, runs after
`mkdocs build --strict` in `make docs-build`, in the docs workflow and in the
pre-push MkDocs hook, and fails closed (exit 3) when Node.js is missing. Its
tests include a planted broken formula that must fail the check.

`scripts/docs/mkdocs_math_hook.py` puts the dollar signs back on the frozen
pages, so a shell variable or a price there is never read as math.

The user-facing documentation is converted. Frozen or generated text is not:
Accepted ADR bodies, `docs/research/`, `docs/changelog-archive/`,
`docs/state.md`, the rebase notes and the generated pages. Only real math
becomes math. Identifiers, option names and a C expression quoted from the
source stay code; a heading stays free of math so that its anchor does not
change. The syntax and the check are described in
[Writing math](../development/docs-site-design.md#writing-math).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| KaTeX, self-hosted (chosen) | Small (about 0.6 MB with fonts), fast, synchronous render, the same compiler runs in Node for the build check, MIT | Covers a TeX subset | The subset covers every formula in the docs; the build check reports an unsupported command |
| MathJax, self-hosted | Wider TeX coverage, accessible output options | About ten times the files, asynchronous typesetting that has to be reset on every instant navigation, slower pages | Coverage the docs do not need |
| Render to SVG or HTML at build time | No JavaScript at read time; fixed output | Needs a Node step inside the MkDocs build or a plugin, a second toolchain to pin, and the sources then differ from what GitHub shows | Adds a build dependency to remove a runtime one the site already accepts for charts |
| KaTeX or MathJax from a CDN (Material's documented recipe) | One line of configuration | A third-party host on every page view; no pin the repository can verify | Breaks the rule that the site loads nothing from another host (HISS-11) |
| Leave formulas as code text | No work | Unreadable structure; cannot be told from quoted source | The reason for the decision |

## Consequences

- **Positive**: formulas render as math in light and dark themes; the source
  stays plain TeX that GitHub also renders; an invalid formula fails the docs
  job; no new network host; the vendoring follows the pattern of the fonts and
  the chart bundle.
- **Negative**: the docs job needs Node.js (pinned with `actions/setup-node`, version 24.21.0; the check
  fails without it). A page author must know the dollar-sign rules and keep
  math out of headings. The converted pages are harder to read in a plain
  text editor than the code text was.
- **Neutral / follow-ups**: a KaTeX update is `vendor_katex.py` against a new
  tarball and a new `vendor.json`. New pages with formulas follow Writing math.
  A page converted by hand later needs no change to the tooling.

## Supply-chain impact

- **New dependencies**: KaTeX 0.18.9 (`docs`, MIT, <https://github.com/KaTeX/KaTeX>),
  vendored as files, not installed. Node.js is a check-time dependency of the
  docs job only.
- **Build-time fetches**: none. The tarball is fetched by hand when KaTeX is
  updated and refused unless its SHA-256 and npm integrity match `vendor.json`.
- **CVE surface delta**: KaTeX runs with `trust: false` in the check and with
  its default (no `\href`, `\url` or `\includegraphics` of unsafe schemes) on
  the site; no new network listener.

## References

- Maintainer decision Q-293 (KaTeX, self-hosted and pinned, no CDN; `$...$`
  and `$$...$$` through `pymdownx.arithmatex`; a strict build check with a
  negative test), recorded in the maintainer's local decision log.
- Maintainer decision Q-294 (convert all user-facing documentation; leave the
  frozen and generated files; only real math becomes math), same log.
- [ADR-1508](1508-docs-site-toolchain-and-charts.md): the self-hosted fonts and
  chart runtime this follows.
- [Material for MkDocs: Math](https://squidfunk.github.io/mkdocs-material/reference/math/):
  the documented KaTeX recipe for `pymdownx.arithmatex` in generic mode.
