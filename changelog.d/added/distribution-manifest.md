- **One distribution manifest for every artifact and channel (ADR-3317, #2314).**
  `packaging/manifest.toml` lists each shipped artifact once (files, platform,
  ABI, backends, licence, signing, SBOM, producing job) and each channel by the
  artifacts it carries. `scripts/release/distribution.py generate` writes the
  Homebrew formula, the conda-forge recipe, the winget manifests and a JSON
  index per channel; `distribution.py check` fails CI when a channel names an
  artifact no pipeline builds or a licence differs from the licensing record
  ([Distribution manifest](docs/development/distribution.md)).
