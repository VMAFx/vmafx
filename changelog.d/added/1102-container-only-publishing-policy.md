Added container-only canonical artifact publishing policy (Phase 4b.9, ADR-1102):
the `vmaf-dev-mcp` container is now the exclusive source for release binaries,
published container images (`ghcr.io/vmafx/vmafx:*`), and CI benchmark/snapshot
artifacts. Host-side builds remain available for IDE/clangd, debugger, and sanitizer
workflows but must not produce published artifacts. Policy documented in
`docs/development/publishing.md`; CLAUDE.md §15 updated with a new publishing bullet.
