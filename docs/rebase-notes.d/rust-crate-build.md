## Rust crate discovery and Go tag policy (2026-10-28)
<!-- Copyright 2026 Lusoris -->
<!-- SPDX-License-Identifier: EUPL-1.2 -->

- Preserve `vmafx-sys/build_support/library_search.rs` and its integration
  tests: ordered unique prefixes, complete header/library pairs, named
  missing-install error, and `DOCS_RS` tree-header selection without linking.
- Keep the stale-header check and bindgen allowlist unchanged. No
  `pkg-config` dependency was added; it is absent from the workspace lockfile.
- Keep `rustdoc::invalid_rust_codeblocks` scoped to the generated bindings
  module: preserved C Doxygen examples are not Rust snippets.
- Preserve the docs.rs package metadata and navigation
  to `docs/api/rust.md` and `docs/usage/go.md`. Registry publication and
  standalone package headers remain separate work.
- Keep the root Go module path and retract block, and the inherited-tag
  guards established by ADR-1805; no tags change in this work.
