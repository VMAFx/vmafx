- **Data-only fixture prereleases now carry build provenance.** A
  `fixtures-*` prerelease without a signed asset lowered the Scorecard
  Signed-Releases score; `fixture-release-provenance.yml` attests the published
  asset after checking its sha256 and uploads the bundle
  ([release guide](docs/development/release.md)).
