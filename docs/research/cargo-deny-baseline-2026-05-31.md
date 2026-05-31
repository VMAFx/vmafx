<!-- markdownlint-disable MD060 -->
# Research digest — cargo-deny baseline scan (2026-05-31)

**Scope:** Initial dependency-graph audit of the Rust workspace
(`bindings/rust/vmafx-sys`, `core/src/feature/rust/tad`) ahead of
adopting `cargo-deny` as a CI gate. Pairs with
[ADR-0917](../adr/0917-cargo-deny-supply-chain-policy.md).

## Method

```bash
cargo install cargo-deny --locked   # cargo-deny 0.19.8
cargo deny init                     # generate template
# Customise deny.toml for vmafx policy (see ADR-0917 §Decision)
cargo deny check
```

Targets enumerated in `deny.toml`:
`x86_64-unknown-linux-gnu`, `aarch64-unknown-linux-gnu`,
`x86_64-pc-windows-msvc`, `x86_64-apple-darwin`,
`aarch64-apple-darwin`.

## Results

### Final scan (after policy + workspace fixes applied)

```text
advisories ok, bans ok, licenses ok, sources ok
```

Three duplicate-version warnings (warn-only, not gating):

| Crate | Versions | Root cause |
|---|---|---|
| `linux-raw-sys` | 0.4.15, 0.12.1 | `bindgen 0.69` pulls `rustix 0.38` → `linux-raw-sys 0.4`; `cbindgen 0.27` pulls `rustix 1.1` → `linux-raw-sys 0.12`. |
| `rustix` | 0.38.44, 1.1.4 | Same: `which 4.4` (via bindgen) pins old; `tempfile 3.27` (via cbindgen) pins new. |
| `windows-sys` | 0.59.0, 0.61.2 | Old via `bindgen` → `which 4.4` → `errno 0.3` → `windows-sys 0.59`; new via `cbindgen` → `clap 4.6` → `anstream 1.0` → `windows-sys 0.61`. |

All three resolve once `bindgen` ≥ 0.70 lands (already on the renovate
backlog). Not blocking; warn-only is the correct policy.

### Issues found and fixed in this PR

1. **`cbindgen` is `MPL-2.0` (weak file-scope copyleft).** Initially
   failed the license check. Resolved by an `[[licenses.exceptions]]`
   entry scoped to `cbindgen` only — it is a build-time tool that
   generates C headers, never linked into the shipping artifact.
2. **`vmafx-tad` declares `BSD-3-Clause-Plus-Patent`.** The SPDX
   short identifier exists but cargo-deny's parser (via the `spdx`
   crate at the version vendored in cargo-deny 0.19.8) does not yet
   recognise it. Result: cargo-deny flagged the workspace crate as
   `unlicensed`. Resolved by adding `publish = false` to
   `core/src/feature/rust/tad/Cargo.toml` — the crate was never
   intended for crates.io publishing (it is consumed in-tree by the
   Meson build via cbindgen), so `[licenses.private] ignore = true`
   correctly skips it.

### Policies validated

- **License allowlist** — every transitive license in the resolved
  graph is in `Apache-2.0`, `Apache-2.0 WITH LLVM-exception`,
  `BSD-3-Clause`, `ISC`, `MIT`, `Unicode-3.0`, or `Unlicense`. No
  GPL/AGPL/LGPL/MPL/SSPL/CDDL/EPL contamination.
- **Bans** — no `openssl-sys` or `native-tls` in the resolved graph.
- **Advisories** — RustSec DB clean. No vulnerabilities, no unsound
  findings, no yanked crates, no unmaintained advisories.
- **Sources** — every crate sourced from
  `https://github.com/rust-lang/crates.io-index`. No git
  dependencies.

## Reproducer

```bash
git checkout chore/cargo-deny-config
cargo install cargo-deny --locked
cargo deny check
# expect: advisories ok, bans ok, licenses ok, sources ok
# expect: 3 duplicate-version warnings (linux-raw-sys, rustix, windows-sys)
```

## Follow-ups

- Re-run after every `bindgen` / `cbindgen` major bump; the
  duplicate-version warnings should collapse when both crates land on
  matching `rustix` minors.
- When a future Rust pilot lands, audit any new transitive deps with
  `cargo deny check --hide-inclusion-graph` first to keep policy
  changes co-located with the PR introducing them.
- If `cargo-deny` ships an SPDX-list update that recognises
  `BSD-3-Clause-Plus-Patent`, consider letting `vmafx-tad` opt back
  out of `publish = false` if a publish use-case ever materialises
  (not currently planned).
