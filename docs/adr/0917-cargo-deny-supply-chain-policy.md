<!-- markdownlint-disable MD060 -->
# ADR-0917: cargo-deny supply-chain policy enforcement

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: `security`, `ci`, `rust`, `supply-chain`, `license`

## Context

The fork's Rust workspace (`bindings/rust/vmafx-sys`,
`core/src/feature/rust/tad`) currently has no automated supply-chain
gate. Today nothing prevents a transitive dependency from pulling in:

- a GPL/AGPL/SSPL crate that contaminates the BSD-3-Clause-Plus-Patent
  shipping license,
- a banned-by-policy crate such as a second TLS stack
  (`openssl-sys` / `native-tls`) when we already link OpenSSL on the C
  side and prefer rustls in Rust,
- a known-vulnerable crate version flagged in the RustSec advisory DB,
- a crate fetched from an unknown git registry or unpinned `git = "…"`
  source.

The Rust pilot crates are small today (vmafx-sys + vmafx-tad), but
Phase 4 of the rebrand adds more Rust surfaces. Wiring the gate now
— while the dep graph is shallow — gives us a clean baseline;
retrofitting later means triaging dozens of legacy duplicates.

[`cargo-deny`](https://embarkstudios.github.io/cargo-deny/) is the
de-facto enforcement tool for these four concerns (licenses, bans,
advisories, sources) and integrates cleanly with the existing
`rust-ci.yml` workflow. The tool is permissively licensed (Apache-2.0
OR MIT) and used by the Rust Foundation, Bevy, Tokio, and others.

## Decision

We will adopt `cargo-deny` with a `deny.toml` at the workspace root
that:

1. **Licenses**: explicit allowlist of permissive licenses only
   (`Apache-2.0`, `Apache-2.0 WITH LLVM-exception`, `BSD-3-Clause`,
   `ISC`, `MIT`, `Unicode-3.0`, `Unlicense`). Everything else is
   rejected by default. `MPL-2.0` is allowed *narrowly* for
   `cbindgen` (build-time tool, no shipping linkage). Workspace crates
   without `publish = false` previously failed because cargo-deny's
   SPDX parser does not yet recognise `BSD-3-Clause-Plus-Patent`; we
   discharge this by adding `publish = false` to `vmafx-tad`, which
   then falls under `[licenses.private] ignore = true`.
2. **Bans**: deny `openssl-sys` and `native-tls` (rustls preferred);
   wildcards (`*`) denied; multiple-versions surfaced as warnings (so
   we see the duplicate-dep surface without failing CI on routine
   transitive churn).
3. **Advisories**: RustSec database; vulnerabilities and unsound
   findings fail the gate (schema v2 default); unmaintained and yanked
   crates warn-only.
4. **Sources**: only `crates.io` allowed; unknown registries and
   unknown git sources fail the gate.

The check runs as a new `cargo-deny` job in `rust-ci.yml`, in parallel
with the existing `vmafx-sys` build/test job, via the official
`EmbarkStudios/cargo-deny-action@v2.0.20` pinned to its commit SHA.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `cargo-deny` (chosen) | Single tool covers all 4 concerns; widely adopted; declarative TOML config; pinned action with SHA. | Yet another CI step; SPDX parser lags behind the registry (forced the `publish = false` workaround). | — |
| `cargo audit` only | Smaller scope, easier to adopt. | Covers advisories only — leaves licenses, bans, and sources unchecked. | Insufficient coverage. |
| GitHub Dependabot + Dependency Review action | Native to GitHub; zero config. | Covers advisories + license review, but Dependency Review only triggers on PRs against the default branch (misses pushes to feature branches); no opinionated ban support; no source-pinning. | Misses the bans + sources surface. |
| `cargo-vet` | Crowdsourced trust audits, stronger supply-chain story than `cargo-deny`. | Requires an audit-set bootstrap and ongoing maintainer attention. Better as a *later* layer on top. | Out of scope for the initial gate; revisit when the Rust surface grows past ~20 crates. |
| Do nothing | No CI cost. | Leaves the four supply-chain risks unmitigated; retrofitting is harder once the dep graph deepens. | Unacceptable risk for a security-sensitive fork. |

## Consequences

- **Positive**:
  - Every PR touching Rust gets license, ban, advisory, and source
    enforcement before merge.
  - License drift is caught at PR time instead of release-prep time.
  - Future Rust pilots inherit the policy automatically (no per-crate
    opt-in required).
  - Pins the dep-graph topology so adding `openssl-sys` (or any other
    banned crate) requires an explicit ADR-cited exception.
- **Negative**:
  - One extra CI job per Rust-touching PR (~20-30 s on cached runs).
  - When upstream Rust crates change their license (rare), the gate
    will fail until `deny.toml` is updated.
  - `BSD-3-Clause-Plus-Patent` is not an SPDX short identifier that
    cargo-deny's parser recognises today — we worked around it by
    making `vmafx-tad` `publish = false`. If a future workspace crate
    needs to publish with that license, it will need a
    `[[licenses.clarify]]` entry with a `LICENSE` file hash anchor.
- **Neutral / follow-ups**:
  - `bindings/rust/vmafx-sys/Cargo.toml` already declares
    `BSD-3-Clause`; no change needed there. We are *not* adding
    `publish = false` to vmafx-sys because the policy intent for it
    may differ (FFI shim could plausibly be published as a public
    crate later); the current SPDX string already validates.
  - Three duplicate-version warnings exist today (`linux-raw-sys`,
    `rustix`, `windows-sys` — all transitive through
    `bindgen`/`cbindgen`). These are warn-only and not actionable
    until bindgen 0.69 → 0.70+ migration aligns its `rustix` minor.
    Tracking via this ADR's References section; no separate issue.
  - Doc: `docs/development/cargo-deny.md` covers running the gate
    locally and triaging findings.

## References

- Source: `req` — paraphrased: the user requested wiring
  `cargo-deny` for Rust supply-chain policy enforcement covering
  licenses, banned crates, vulnerability scanning, duplicate-dep
  detection, and source pinning.
- [cargo-deny user guide](https://embarkstudios.github.io/cargo-deny/)
- [SPDX license list](https://spdx.org/licenses/)
- [RustSec advisory database](https://rustsec.org/)
- Related: [ADR-0535](0535-adr-atomic-allocator.md) — ADR atomic allocator
- Related: [ADR-0628](0628-adr-allocator-remote-aware.md) — remote-aware allocator
- Initial findings: `cargo deny check` on this PR reports
  `advisories ok, bans ok, licenses ok, sources ok`; 3 duplicate-version
  warnings (warn-only).
