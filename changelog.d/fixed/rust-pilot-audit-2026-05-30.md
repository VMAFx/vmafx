- Cargo.lock for the Rust workspace (`vmafx-sys` FFI bindings,
  `vmafx-tad` cbindgen pilot) is now in sync with the resolved
  build-dependency graph. The committed lockfile predated the
  `[build-dependencies]` for `bindgen` 0.69 and `cbindgen` 0.27 and
  their transitive crates (`clang-sys`, `cexpr`, `nom`, `rustix`,
  `windows-sys` 0.59 family, 27 others), so `cargo build` would
  silently mutate `Cargo.lock` on every fresh checkout. Audit pass
  refreshed the lock; no version churn for already-pinned entries,
  no MSRV impact, no security advisories from `cargo audit`. The
  workspace passes `cargo build`, `cargo test`
  (15 tests / 0 failures), `cargo clippy --all-targets -- -D warnings`,
  `cargo fmt --check`, and `cargo audit` cleanly.
