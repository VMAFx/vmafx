- **`cargo-deny` Rust supply-chain policy
  ([ADR-0917](../docs/adr/0917-cargo-deny-supply-chain-policy.md)).**
  New `deny.toml` at the workspace root enforces a permissive-license
  allowlist (Apache-2.0, BSD-3-Clause, ISC, MIT, Unicode-3.0,
  Unlicense; `MPL-2.0` allowed narrowly for `cbindgen` build-time
  only), bans `openssl-sys` and `native-tls` (rustls preferred),
  scans the RustSec advisory DB (vulnerabilities fail; yanked +
  unmaintained warn), and restricts crate sources to `crates.io`
  (unknown registries and unknown git sources denied). Runs as a
  parallel `cargo-deny` job in `rust-ci.yml` via the official
  `EmbarkStudios/cargo-deny-action@v2.0.20` action (pinned by SHA).
  Initial baseline: `advisories ok, bans ok, licenses ok, sources
  ok`; 3 known duplicate-version warnings (`linux-raw-sys`, `rustix`,
  `windows-sys`, all transitive via `bindgen`/`cbindgen`). User-facing
  guide: [`docs/development/cargo-deny.md`](../docs/development/cargo-deny.md).
