---
paths:
  - scripts/ci/check_release_provenance_assets.py
  - scripts/ci/tests/test_check_release_provenance_assets.py
invariant: Releasers upload build provenance as *.intoto.jsonl release assets (Scorecard 10).
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# Release build provenance assets (ADR-3316)

OpenSSF Scorecard Signed-Releases check (scorecard v5.6.0) evaluates signature
assets (.sigstore.json) to 8, in-toto build provenance (*.intoto.jsonl) to 10.
Every release-publishing workflow writes normalized single-line in-toto SLSA
attestation bundle and uploads it:

- `supply-chain.yml`: `vmafx-build-provenance.intoto.jsonl` (native assets) + `vmaf-mcp-provenance.intoto.jsonl` (PyPI package).
- `macos-tester-bundle.yml`: `vmafx-tester-macos-provenance.intoto.jsonl`.
- `windows-tester-bundle.yml`: `vmafx-tester-windows-provenance.intoto.jsonl`.

`scripts/ci/check_release_provenance_assets.py` parses `.github/workflows/` with
`yaml.safe_load`. Detects releaser steps (`softprops/action-gh-release` or `gh release create/upload`),
asserts `.intoto.jsonl` write in `run:` step (`jq --compact-output . > ...intoto.jsonl`) and presence
in uploaded asset set (glob match or `files:` list). Exempt list allows non-binary
companion workflows (`published-rc-licence-companions.yml`); stale exemptions fail gate.
Verification: `gh attestation verify <asset> --bundle <bundle>.intoto.jsonl --repo VMAFx/vmafx`.
