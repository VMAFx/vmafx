- **Renovate's Rust toolchain bump passes the deliverables gate again.**
  `scripts/ci/classify-dependency-pr.sh` treats the root `rust-toolchain.toml`
  (ADR-3318) as a dependency pin, so a bot pull request that only moves the
  toolchain no longer needs the deep-dive checklist. A nested
  `rust-toolchain.toml` and a human author stay gated.
