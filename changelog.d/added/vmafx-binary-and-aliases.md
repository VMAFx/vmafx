### `vmafx` binary and AI tool aliases (ADR-0690)

- Install `vmafx` as a symlink to `vmaf` in the same `bindir` via Meson
  `install_symlink()`. No separate binary; one binary, one symlink.
- When invoked as `vmafx`, the binary applies modernized defaults:
  `--precision=max` (IEEE-754 `%.17g` lossless output) and a distinct startup
  banner `VMAFX version <V> (precision=max)`.
- `--version` reports `VMAFX <V> (auto-backend, precision=max)` in vmafx mode.
- All existing `vmaf` flags are accepted unchanged; `--precision=legacy`
  restores the `%.6f` output in vmafx mode.
- Add `vmafx-train` console-script alias (`ai/pyproject.toml`) pointing to
  `vmaf_train.cli:app`.
- Add `vmafx-tune` console-script alias (`tools/vmaf-tune/pyproject.toml`)
  pointing to `vmaftune.cli:main`.
- Add `vmafx-mcp` console-script alias (`mcp-server/vmaf-mcp/pyproject.toml`)
  pointing to `vmaf_mcp.server:main`.
- Docs: `docs/usage/vmafx-cli.md`.
