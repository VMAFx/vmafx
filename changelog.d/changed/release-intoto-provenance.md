- **Release workflows publish build-provenance assets as `*.intoto.jsonl`.**
  Every workflow that creates a release or uploads release assets (`supply-chain.yml`,
  `macos-tester-bundle.yml`, `windows-tester-bundle.yml`) normalizes its GitHub-native
  attestation bundle with `jq -c .` into a single-line `*.intoto.jsonl` file and attaches
  it as a release asset. This satisfies OpenSSF Scorecard Signed-Releases criteria for a
  score of 10 while keeping all existing `.sigstore.json` signature assets intact.
  `scripts/ci/check_release_provenance_assets.py` gates that releaser workflows write and
  upload matching `.intoto.jsonl` provenance assets (ADR-3316).
