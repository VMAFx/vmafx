- **Dependency bumps can merge again.** No Renovate PR had landed since
  2026-09-10: twenty were open, two of them security fixes. `draftPR: true`
  (PR #1411) made every bot PR a draft, and ADR-0679 makes the single required
  `Required Checks Aggregator` context **fail** on drafts by design, so
  `automerge` could never fire and nothing promoted the drafts — there is no
  workflow or script in the tree that marks a PR ready for review. `draftPR` is
  now `false` on the six `packageRules` that set `automerge: true` (GitHub
  Actions minor+patch, pre-commit revisions, Python patch, Go minor+patch,
  Cargo minor+patch, Docker digests) and on `vulnerabilityAlerts`, so a
  security bump is mergeable the moment it opens while still requiring a human
  to approve it. Everything else keeps opening as a draft and stays out of the
  ready queue, which is what PR #1411 wanted. Verified with
  `renovate-config-validator` from Renovate 44.93.4 — pin the version, because a
  stale `npx` cache resolves 37.440.7 and reports 17 false errors on this file.
  ADR-1251, research digest `docs/research/2059-why-no-dependency-bump-can-merge.md`.
