## Release provenance assets (2026-10-14)

- `.github/workflows/supply-chain.yml` normalizes and uploads `vmafx-build-provenance.intoto.jsonl`
  and `vmaf-mcp-provenance.intoto.jsonl` as release assets alongside existing `.sigstore.json` bundles.
- `.github/workflows/macos-tester-bundle.yml` normalizes and attaches `vmafx-tester-macos-provenance.intoto.jsonl`.
- `.github/workflows/windows-tester-bundle.yml` normalizes and attaches `vmafx-tester-windows-provenance.intoto.jsonl`.
- `scripts/ci/check_release_provenance_assets.py` statically validates that every workflow that
  invokes `action-gh-release` or runs `gh release create`/`upload` writes and uploads an `.intoto.jsonl` asset.
- **On sync**: upstream Netflix does not attach GitHub-native build-provenance bundles or in-toto
  provenance assets to its release pipeline. When merging upstream release or packaging workflows,
  preserve the `actions/attest-build-provenance` steps, `.intoto.jsonl` staging commands, and
  release asset upload lists (ADR-3316).
