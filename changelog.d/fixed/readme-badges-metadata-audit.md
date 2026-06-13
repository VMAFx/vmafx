- **README badge audit + Cargo / pyproject repo-metadata sweep.** Added
  Rust CI and Go CI workflow badges to `README.md` (both workflows ship
  on master but were not surfaced in the badge row). Backfilled
  `[workspace.package]` in the root `Cargo.toml` with the canonical
  `VMAFx/vmafx` `repository` / `homepage` / `documentation` URLs +
  `license` + `authors`; the two workspace member crates
  (`bindings/rust/vmafx-sys`, `core/src/feature/rust/tad`) switched to
  workspace-inherited metadata so the URLs stay consistent across the
  Rust workspace. Added `[project.urls]` (`Homepage` / `Repository` /
  `Documentation` / `Issues` / `Changelog`) to every fork-authored
  `pyproject.toml` that ships a `[project]` block (root tooling,
  `ai/`, `tools/vmaf-tune`, `tools/vmaf-roi-score`,
  `tools/ensemble-training-kit`, `dev-llm/`, `mcp-server/vmaf-mcp/`).
  `deploy/helm/vmafx/Chart.yaml` was already correct (`home` +
  `sources` point at `VMAFx/vmafx`); no change. The five existing
  workflow badges in `README.md` already pointed at the correct
  `VMAFx/vmafx` repo and reference real, active, master-green
  workflows; verified via the GitHub Actions API.
