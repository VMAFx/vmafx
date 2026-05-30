- **Dependency security sweep (2026-05-29)** — audited Rust (`cargo audit`,
  103 crates) and Python (`pip-audit`, all in-tree requirements files) for
  known CVEs and RustSec advisories: **0 vulnerabilities found** in either
  ecosystem. Also repaired an incomplete `Cargo.lock` that was missing the
  `bindgen` subtree (30 packages), which had been silently preventing
  `cargo audit` from running. See [ADR-0856](docs/adr/0856-security-advisory-sweep.md).
