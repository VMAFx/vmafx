### Bundled doc/state/ADR sweep — batch 1 (#327, #330, #336, #383, #337)

Bundle of five docs-only and metadata PRs applied as a single squash onto
master tip `40d192ef1`:

- **#327 — Doxygen comments on 15 undocumented public C-API entry points**
  (`core/include/libvmaf/feature.h`, `model.h`, `dnn.h`): round-2 follow-on
  to PR #302; adds `@brief`, `@param`, `@return`, and ownership/lifetime
  contracts to public surfaces consumed by the FFmpeg patch stack, the
  Go/Rust bindings, and the MCP server.

- **#330 — Go test-coverage expansion for `cmd/vmafx-{controller,server,mcp}`**:
  adds `main_extra_test.go`, `nodes/registry_edge_test.go`, and
  `impl_test.go`; coverage deltas: controller 18.6 → 32.4 %, server
  27.5 → 47.9 %, MCP 3.5 → 24.6 %. Also fixes `.gitignore` to anchor
  binary-ignore rules with a leading `/`.

- **#336 — `docs/state.md` closed-PR row sweep**: reconciles 2 Open rows
  that cited CLOSED-not-merged PRs (#214, #215). Net Open count −1.

- **#383 — README badge audit + Cargo / pyproject repo-metadata sweep**:
  adds Rust CI and Go CI badges to README; wires `[workspace.package]`
  metadata into all Rust crates; adds `[project.urls]` to all 7
  fork-authored `pyproject.toml` files.

- **#337 — ADR-0865: Sunset ANSNR (pre-VMAF metric)**: authors the missing
  parent ADR for PR #38's `float_ansnr` removal; back-dated to 2026-05-28
  (PR #38 merge date). Regenerates `docs/adr/README.md` and
  `_index_fragments/_order.txt`.
