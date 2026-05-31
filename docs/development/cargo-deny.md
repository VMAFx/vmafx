<!-- markdownlint-disable MD060 -->
# cargo-deny — Rust supply-chain policy

The fork enforces a supply-chain policy on the Rust workspace
(`bindings/rust/vmafx-sys`, `core/src/feature/rust/tad`, future Rust
pilots) via [`cargo-deny`](https://embarkstudios.github.io/cargo-deny/).
The policy lives in `deny.toml` at the workspace root and runs on
every PR that touches Rust files via the `cargo-deny` job in
[`.github/workflows/rust-ci.yml`](../../.github/workflows/rust-ci.yml).
See [ADR-0917](../adr/0917-cargo-deny-supply-chain-policy.md) for the
decision rationale and alternatives considered.

## What the gate checks

| Check | Behaviour | Failure mode |
|---|---|---|
| `licenses` | Allowlist of permissive SPDX identifiers (`Apache-2.0`, `BSD-3-Clause`, `ISC`, `MIT`, `Unicode-3.0`, `Unlicense`, plus `Apache-2.0 WITH LLVM-exception`). Per-crate exceptions: `cbindgen` (MPL-2.0, build-time only). Private (`publish = false`) workspace crates are skipped. | Fails the gate. |
| `bans` | Denies `openssl-sys` and `native-tls` (rustls preferred). Denies wildcard (`*`) version requirements. Surfaces duplicate-version transitives as warnings. | `deny` entries fail the gate; duplicates are warn-only. |
| `advisories` | Pulls the RustSec advisory DB. Schema v2: vulnerabilities and unsound findings fail. Yanked and unmaintained crates warn-only. | Vulnerability findings fail the gate. |
| `sources` | Only `crates.io` allowed. Unknown registries and unknown git sources are denied. | Fails the gate. |

## Running locally

```bash
# Install (once)
cargo install cargo-deny --locked

# Full check from the workspace root
cargo deny check

# Single-section runs (faster iteration)
cargo deny check licenses
cargo deny check bans
cargo deny check advisories
cargo deny check sources
```

Expected output on a clean tree:

```text
advisories ok, bans ok, licenses ok, sources ok
```

with up to three duplicate-version warnings (warn-only):
`linux-raw-sys`, `rustix`, `windows-sys` — all transitive through
`bindgen`/`cbindgen`. These are tracked under ADR-0917 and do not
gate the merge.

## Adding a dependency that trips the gate

The three most common cases:

1. **License not in the allowlist.** If the license is permissive and
   uncontroversial (Boost, Zlib, MIT-0, …), add it to the `allow = [
   … ]` list in `deny.toml` with an inline comment naming the crate
   that pulled it in. If it is copyleft (MPL-2.0, LGPL-*, etc.),
   discuss in the PR before adding an `[[licenses.exceptions]]`
   entry; cite the ADR or research digest that approves it.
2. **Banned crate dragged in transitively.** Audit the upstream
   crate's feature flags — most TLS-touching deps default to
   `native-tls` but ship a `rustls` opt-in. Switch features in
   `Cargo.toml` rather than removing the ban.
3. **Yanked / unmaintained advisory.** Bump the crate to a maintained
   version. If no upstream replacement exists, add an
   `ignore = [{ id = "RUSTSEC-NNNN-NNNN", reason = "…" }]` entry
   with a link to the ADR or research digest that documents the
   acceptance.

## Notes on the workspace's own licenses

- `vmafx-sys` declares `BSD-3-Clause` — recognised by cargo-deny's
  SPDX parser, so it passes the license check directly.
- `vmafx-tad` declares `BSD-3-Clause-Plus-Patent`. The SPDX short
  identifier exists but cargo-deny's parser does not recognise it
  yet, so the crate is marked `publish = false` to fall under
  `[licenses.private] ignore = true`. If you add a new pilot crate
  using the fork license, follow the same pattern.

## Related

- [ADR-0917](../adr/0917-cargo-deny-supply-chain-policy.md) — policy decision
- [ADR-0707](../adr/0707-tad-cbindgen-pilot.md) — TAD cbindgen pilot
- [ADR-0702](../adr/0702-vmafx-sys-ffi-crate.md) — vmafx-sys FFI crate
- [`deny.toml`](../../deny.toml) — the live configuration
- Upstream: <https://embarkstudios.github.io/cargo-deny/>
