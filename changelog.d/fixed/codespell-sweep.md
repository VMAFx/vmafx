- **docs/lint**: project-wide `codespell` sweep (ADR-0910).
  Adds `.codespellrc` at repo root pinning the skip list
  (Netflix-author / vendored / frozen-ADR files), the
  ignore-words list (domain acronyms `ANE`/`HSA`/`SME`/`CANN`/
  `COO`/`HSI`/`ND`/`BU`, Linux device-node fragment `renderD*`,
  SIMD lane variables `thi`/`tlo`, Go/Python variable
  conventions `disPath`/`fo`/`aks`/`dout`/`iterm`, valid English
  hyphenations `re-use*`/`re-declare`/`pre-emptive*`, verbatim
  quotes `entirity`/`arent`, and project shorthand
  `recal`/`padd`/`browseable`/`unparseable`/`statics`/`ist`).
  Fixes three real typos: `orginization` → `organization` in
  `CONTRIBUTING.md`; `brigher` → `brighter` and `Visibilty` →
  `Visibility` in `docs/metrics/cambi.md`. Run `codespell`
  locally to surface future regressions; CI integration is a
  follow-up.
