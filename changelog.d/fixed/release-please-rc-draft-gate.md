- An unpublished release-candidate draft now pauses release-please. The
  draft check in `.github/workflows/release-please.yml` matched only plain
  `vX.Y.Z` tags, so a waiting `v1.0.0-rc.1` draft was invisible and the next
  push to `master` would have re-run release-PR generation against an untagged
  release. It now accepts the same `vX.Y.Z` / `vX.Y.Z-rc.N` shapes as
  `scripts/release/verify-release-version.sh`, and it fails closed when the
  release list cannot be read.
