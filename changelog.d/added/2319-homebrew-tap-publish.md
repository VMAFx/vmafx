- **The release publishes a Homebrew formula to `Vidiko/homebrew-tap` (ADR-3387).**
  After a final release, the supply-chain workflow opens a pull request with
  `Formula/vmafx.rb` on the tap; once it merges, `brew install Vidiko/tap/vmafx`
  builds VMAFx from the release source archive
  ([Homebrew tap](docs/development/distribution.md#homebrew-tap)).
