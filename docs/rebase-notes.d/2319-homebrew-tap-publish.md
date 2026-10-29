## Homebrew tap publish job (2026-10-29)

- `.github/workflows/supply-chain.yml`: new `homebrew-tap` job after
  `attach-to-release`. Fork-only, nothing upstream to merge.
- `scripts/release/distribution.py`: `homebrew` and `source-url` subcommands;
  `scripts/release/publish_homebrew_tap.py` (new). **On sync**: none of these
  files exist upstream.
