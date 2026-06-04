### docs: scrub residual `libvmaf/` paths after ADR-0700 rename

All doc and AGENTS.md references to the old `libvmaf/` source directory
have been updated to `core/` following the ADR-0700 rename. Affected
surfaces: `docs/usage/`, `docs/metrics/`, `docs/ai/`, `docs/backends/`,
`docs/getting-started/` (install guides + build-from-source),
`docs/architecture/`, `AGENTS.md`, `CONTRIBUTING.md`, and the comment in
`core/include/libvmaf/libvmaf_mcp.h`. The `compat/python-vmaf/` workspace
and config paths are also updated in `docs/architecture/workspace.md`.
