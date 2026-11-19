---
paths:
  - .github/workflows/*.yml
  - .github/PULL_REQUEST_TEMPLATE.md
invariant: Preserve fork-added workflows; prefer fork version on clashes; never overwrite PULL_REQUEST_TEMPLATE.md.
---
# Upstream-merge guidance

Netflix/vmaf ships its own workflows under `.github/workflows/`
(CI, release, etc.). Fork's workflows live alongside them; file
collisions are rare because fork-added workflow names
(`rule-enforcement.yml`, `nightly-bisect.yml`, `supply-chain.yml`,
etc.) don't clash with upstream's names. On sync:

1. Preserve every fork-added workflow verbatim unless ADR that
   introduced it is superseded.
2. For workflows existing in both trees (e.g. `codeql.yml`),
   prefer fork version — usually has stricter pins and
   broader matrix legs.
3. `PULL_REQUEST_TEMPLATE.md` is fork-authored; upstream has none.
   Never overwrite it on sync.
