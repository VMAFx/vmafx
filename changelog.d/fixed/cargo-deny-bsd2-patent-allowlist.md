- **`cargo deny check` now passes for all workspace crates.** ADR-1036 corrected
  the fork's SPDX identifier from `BSD-3-Clause-Plus-Patent` to `BSD-2-Clause-Patent`
  across `Cargo.toml` manifests, but `deny.toml`'s `[licenses].allow` list was not
  updated at the same time. This caused `cargo deny check licenses` to reject the fork's
  own crates (`vmafx`, `vmafx-sys`, `vmafx-tad`) with "rejected: license is not
  explicitly allowed". Adding `"BSD-2-Clause-Patent"` to the allowlist and correcting
  the stale comment + SPDX header in `deny.toml` restores the supply-chain gate.
