# Research: Security Advisory Sweep — 2026-05-29

**Scope**: Rust workspace (`Cargo.lock`) and Python dependency files
(`python/requirements.txt`, `docs/requirements.txt`,
`python/test/requirements.txt`,
`tools/ensemble-training-kit/requirements-frozen.txt`)

**Tools used**

| Tool | Version | Invocation |
|------|---------|------------|
| `cargo-audit` | v0.22.1 | `cargo audit --json` against repaired Cargo.lock |
| `pip-audit` (via uv) | v2.10.0 | `uv tool run pip-audit -r <file> --no-deps` |

---

## Findings

### Rust (Cargo)

**Pre-existing defect discovered**: `Cargo.lock` was missing `bindgen` and its 30
transitive dependencies. `vmafx-sys/Cargo.toml` declares `bindgen = "0.69"` but
the lock file had never been regenerated after the crate was added. `cargo audit`
failed to parse the lock file until `cargo generate-lockfile` was run to repair it.

After repair: **0 vulnerabilities, 0 warnings** across 103 locked crates.

Notable informational items (no security impact — for Renovate):

| Crate | Locked | Latest |
|-------|--------|--------|
| `bindgen` | 0.69.5 | 0.72.1 |
| `cbindgen` | 0.27.0 | 0.29.3 |

### Python

All four requirements files audited against the OSV database via `pip-audit`.
**0 known vulnerabilities found** in any file.

---

## Outcome

No CRITICAL or HIGH security advisories in either ecosystem. The only actionable
fix is the Cargo.lock repair (missing `bindgen` subtree). Non-security version
drift (`bindgen`, `cbindgen`) is tracked for Renovate, not bumped here.

**References**: ADR-0856, PR that carries this sweep.
