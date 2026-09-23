<!-- markdownlint-disable MD013 MD060 -->
# Research digest 2080 — Rust CI path filter coverage for public libvmaf C headers

- **Status**: Completed
- **Date**: 2026-09-23
- **Author**: lusoris
- **Governing issue**: `T-PATH-FILTERS-WEAKEN-NEW-GATES-2026-09-22` (Rust CI path-filter scope / canonical `BUG-098`; closed row `T-RUST-CI-PATH-FILTER-LIBVMAF-HEADERS-2026-09-23`)
- **Governing ADRs**: ADR-1297 (Every check that reports on a PR is required), ADR-0313 (CI required checks aggregator), ADR-0706 (vmafx-sys Rust FFI crate)

## 1. Problem Statement

Under ADR-1297, `vmafx-sys CI` and `cargo-deny` were added to the `Required Checks Aggregator` (`.github/workflows/required-aggregator.yml`).
However, under ADR-0313, a check that does not report on a pull request counts as passed (to allow doc-only PRs without full matrix compilation).
`.github/workflows/rust-ci.yml` had the following path filter triggers on `push` and `pull_request`:

```yaml
paths:
  - "bindings/rust/**"
  - "core/src/feature/rust/**"
  - "Cargo.toml"
  - "Cargo.lock"
  - "deny.toml"
  - ".github/workflows/rust-ci.yml"
```

Crucially, `bindings/rust/vmafx-sys` is a raw FFI binding crate whose `build.rs` compiles libvmaf headers directly using `bindgen`:

```rust
let header = format!("{include_dir}/libvmaf/libvmaf.h");
println!("cargo:rerun-if-changed={header}");
```

When a pull request touches public C library headers under `core/include/libvmaf/**` without touching Rust files:

1. GitHub Actions path filtering skips `.github/workflows/rust-ci.yml`.
2. The `Required Checks Aggregator` polls for reporting checks and treats `vmafx-sys CI` as not applicable (pass).
3. Any syntax errors, signature changes, struct mutations, or enum alterations in `core/include/libvmaf/**` that break Rust FFI generation or smoke tests merge without detection.

This defect was explicitly tracked in `docs/state.md` under `T-PATH-FILTERS-WEAKEN-NEW-GATES-2026-09-22` (canonical `BUG-098`; closed as `T-RUST-CI-PATH-FILTER-LIBVMAF-HEADERS-2026-09-23` while residual workflow path-filter audits remain open) and in comments inside `.github/workflows/rust-ci.yml`.

## 2. Evidence and Measurement

### 2.1 Workflow Trigger Absence

Inspecting `.github/workflows/rust-ci.yml` confirmed:

- `on.push.paths` contained 6 patterns, none covering `core/include/` or `core/include/libvmaf/**`.
- `on.pull_request.paths` contained 6 patterns, none covering `core/include/` or `core/include/libvmaf/**`.
- Job comments on line 39 stated:
  `# Known gap this gate does NOT close: the paths: filter above does not include core/include/libvmaf/**, although the binding binds that header.`

### 2.2 CI Impact Planner Divergence

Testing the planner contract via `_plan_for(["core/include/libvmaf/libvmaf.h"])` in `scripts/ci/tests/test_ci_impact.py` yielded:

- `plan.selectors["c_core"] == True`
- `plan.selectors["rust"] == False` (prior to fix)

Because `.github/ci-impact.json` had:

```json
"rust": {
  "patterns": [
    "bindings/*",
    "bindings/**",
    "core/src/feature/rust/*",
    "core/src/feature/rust/**",
    "Cargo.toml",
    "Cargo.lock",
    "deny.toml"
  ]
}
```

## 3. Decision Matrix and Alternatives

| Alternative | Pros | Cons | Verdict |
|---|---|---|---|
| A. Remove `paths:` filters from `rust-ci.yml` entirely | Runs on every PR, zero bypass risk | Runs 30-minute Rust toolchain and cargo builds on doc-only and Python-only PRs, causing runner queue congestion | Reject |
| B. Add `"core/include/libvmaf/**"` to `paths:` in `rust-ci.yml` and `ci-impact.json` | Exact match for public headers consumed by `vmafx-sys/build.rs`; keeps doc-only PRs fast; closes the documented blind spot | None | **Selected** |
| C. Add `"core/**"` to `paths:` in `rust-ci.yml` | Covers internal C engine edits as well | `vmafx-sys` only tests public C API and bindings; internal C engine is thoroughly gated by `Ubuntu gcc+DNN`, `Ubuntu clang+DNN`, `Sanitizers`, etc. Unnecessarily doubles CI load on internal kernel refactors | Reject |

## 4. Verification

1. Unit tests in `scripts/ci/tests/test_rust_ci_workflow_contract.py` prove:
   - `on.push.paths` in `rust-ci.yml` includes `"core/include/libvmaf/**"`.
   - `on.pull_request.paths` in `rust-ci.yml` includes `"core/include/libvmaf/**"`.
   - `selectors.rust.patterns` in `ci-impact.json` includes `"core/include/libvmaf/**"`.
2. Unit test `test_libvmaf_public_header_change_selects_rust_and_c_core` in `scripts/ci/tests/test_ci_impact.py` proves:
   - Modifications to `core/include/libvmaf/libvmaf.h` activate `plan.selectors["rust"]`.
3. Check scripts:
   - `bash scripts/ci/check-aggregator-names.sh` passes (79 required checks match workflow definitions).
   - `bash scripts/ci/check-state-md-rows.sh` passes (clean rows and status concordance).
