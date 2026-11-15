---
paths:
  - scripts/release/distribution.py
  - scripts/release/lint-distribution-recipes.sh
  - scripts/release/tests/test_distribution.py
  - packaging/manifest.toml
invariant: packaging/manifest.toml = single source for Homebrew, conda-forge, winget; never hand-write.
---
<!-- markdownlint-disable MD013 MD060 -->
# Distribution manifest and recipe generator (`distribution.py`, ADR-3317)

## distribution.py + lint-distribution-recipes.sh (ADR-3317, #2314)

`packaging/manifest.toml` = one source for artifacts + channels. `check`:
licence = licensing-record union / package licence field (reader shared with
`scripts/ci/check_licence_metadata.py`), text in `LICENSES/`, producer job
exists in its workflow, channel names only built artifacts, planned channel
has tracking issue. Producer job renamed in `supply-chain.yml` /
`windows-tester-bundle.yml` / `macos-tester-bundle.yml` -> manifest changes
same PR. `generate` writes Homebrew, conda-forge (rattler v1), winget 1.12.0,
per-channel index. Never hand-write a channel recipe beside it. Linter script
images digest-pinned; brew runs `--network=none`. Test coverage:
`scripts/release/tests/test_distribution.py` (vendored winget schemas,
no-GPU + all-backend boundaries).
