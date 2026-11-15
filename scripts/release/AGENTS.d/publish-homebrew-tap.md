---
paths:
  - scripts/release/publish_homebrew_tap.py
  - scripts/release/tests/test_publish_homebrew_tap.py
  - .github/workflows/supply-chain.yml
invariant: Open signed-off tap PR from distribution.py output on final versions only; never push main.
---
<!-- markdownlint-disable MD013 MD060 -->
# Homebrew tap publisher (`publish_homebrew_tap.py`, ADR-3387)

## publish_homebrew_tap.py + `homebrew-tap` job (ADR-3387, #2319)

Tap formula = `distribution.py homebrew` output (tap REUSE header + generated
formula); digest from the archive `distribution.py source-url` prints, never a
second URL spelling. Script opens a pull request only (branch
`vmafx-<version>`, contents-API commit with `Signed-off-by:`; tap CI refuses a
commit without it); never push to tap default branch. Dry run default,
`--apply` in the job. Final versions only (job `if`). Token = release-bot app,
owner `Vidiko`, repo `homebrew-tap`. Test coverage:
`scripts/release/tests/test_publish_homebrew_tap.py` (fake transport, no
network).
