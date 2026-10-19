## Distribution manifest and channel generators (2026-10-15)

- `packaging/manifest.toml` is the one description of shipped artifacts and
  channels (ADR-3317). A pull request that renames a producer job in
  `supply-chain.yml`, `windows-tester-bundle.yml` or
  `macos-tester-bundle.yml`, changes a licensing record in
  `tools/rc1-tester/image/licensing.json` or a package licence field changes
  the manifest in the same pull request; `distribution.py check` (pre-commit
  hook `distribution-manifest`, required job `Release Script Contract`) fails
  otherwise.
- `packaging/schemas/winget-1.12.0/` is vendored and hash-checked by
  `scripts/docs/check_vendored_assets.py`, which now scans `docs/` and
  `packaging/`. Replace the files with `vendor.json` updated, never edit them.
- No upstream Netflix file is touched.
