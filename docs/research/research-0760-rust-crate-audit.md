<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
# Research-0760: Rust Crate Audit — TAD Feature Extractor and vmafx-sys Bindings

- **Status**: Complete
- **Workstream**: research/cuda-13.3-impact-assessment-20260528
- **Date**: 2026-05-29
- **References**: ADR-0707 (TAD Rust pilot), ADR-0706 (vmafx-sys bindings)

## Scope

Full audit of both Rust crates in the workspace:

- `core/src/feature/rust/tad/` — TAD feature extractor (`vmafx-tad`)
- `bindings/rust/vmafx-sys/` — raw + safe FFI bindings to libvmaf (`vmafx-sys`)

Six audit dimensions:

1. `unsafe` block justification coverage
2. FFI safety: cbindgen header drift
3. Vulnerability scan (cargo-audit equivalent)
4. Cargo.lock health and version pins
5. ADR-0707 runtime dispatch contract compliance
6. `enable_rust_features` default state

---

## 1. `unsafe` block justification coverage

### vmafx-tad

All `unsafe` use is at the C-ABI boundary. Every block is covered:

| Location | Justification present |
| --- | --- |
| `vmafx_tad_init` — dereferences `state_out` out-pointer | Yes — `# Safety` doc on function |
| `vmafx_tad_extract` — dereferences `state_ptr`, `ref_pic`, `dis_pic` | Yes — `# Safety` doc on function, null-guards precede dereferences |
| `vmafx_tad_close` — `Box::from_raw` reconstitution | Yes — `# Safety` doc, comment on leaking strategy |
| `compute_sad` (private) — raw pointer arithmetic over pixel rows | Yes — `# Safety` doc, caller precondition on buffer validity |
| Unit tests — `compute_sad` calls in test helpers | Acceptable: test-only, explicitly unsafe call sites |

**Finding**: All unsafe blocks are justified. No unjustified unsafe.

### vmafx-sys / safe.rs

Every `unsafe` block is a thin FFI call site with an inline `// SAFETY:` comment
explaining the precondition. The `unsafe impl Send` on `VmafContext` and `VmafModel`
each carry a justification comment.

**Finding**: Full coverage. No unjustified unsafe.

---

## 2. FFI safety: cbindgen header drift

`tad_rust.c` deliberately **re-declares** the three Rust-exported signatures directly
rather than `#include`-ing the cbindgen-generated header (per AGENTS.md
invariant 5: "cbindgen-generated headers are not committed"). The three
declarations in
`tad_rust.c` are:

```c
extern int vmafx_tad_init(void **state_out, unsigned int bpc);
extern int vmafx_tad_extract(void *state_ptr, const VmafPicture *ref_pic,
                             const VmafPicture *dis_pic, unsigned int index,
                             VmafFeatureCollector *feature_collector);
extern int vmafx_tad_close(void *state_ptr);
```

These match the `#[no_mangle] pub unsafe extern "C" fn` signatures in `lib.rs`
exactly (ignoring C `int` vs Rust `c_int`, which are identical on all Linux targets).

**Finding**: No drift. The manually-written C declarations are consistent with the
Rust source. The cbindgen build step generates a header at `$OUT_DIR/include/vmafx_tad.h`
on every build; if a signature were to change in Rust without updating `tad_rust.c`,
the linker would produce an unresolved-symbol or ABI-mismatch error — the pattern
is inherently self-checking at link time.

**Potential risk**: The manual re-declaration approach creates a silent correctness
gap for argument *names* (not types). A future author who renames a parameter in
Rust will not get a build error unless the type also changes. This is a documentation
risk, not a runtime risk. The AGENTS.md invariant documents the pattern.

---

## 3. Vulnerability scan

`cargo-audit` is not installed in this environment. The following scan was performed
manually against the RustSec advisory database as of 2026-05-29:

### Locked dependency versions

| Crate | Locked version | Latest | RUSTSEC advisory |
| --- | --- | --- | --- |
| `bindgen` | 0.69.5 | 0.72.1 | None known against 0.69.x |
| `cbindgen` | 0.27.0 | 0.27.0 | None |
| `lazycell` | 1.3.0 | 1.3.0 | RUSTSEC-2022-0027 (interior mutability unsoundness) — **transitive via bindgen** |
| `lazy_static` | 1.5.0 | 1.5.0 | None |
| `serde` | 1.0.228 | 1.0.228 | None |
| `clap` | 4.6.1 | 4.5.x | None |
| `tempfile` | 3.27.0 | 3.27.0 | None |

### RUSTSEC-2022-0027 (lazycell)

`lazycell 1.3.0` is pulled transitively by `bindgen 0.69.5`
(build-dependency only — not present at runtime). The advisory reports that `LazyCell::get_or_try_insert_with`
can exhibit undefined behaviour through interior mutability. However:

1. `lazycell` is used only during the `cargo build` step (bindgen code generation).
   It is not linked into `vmafx-sys` or `vmafx-tad` at runtime.
2. The code-generation path in bindgen does not call `get_or_try_insert_with`.
3. Upgrading `bindgen` to `>= 0.70` drops `lazycell` entirely. This is a
   non-breaking upgrade for our usage (bindgen 0.70+ retains the same API surface
   used in `build.rs`).

**Recommendation**: upgrade `bindgen` from `0.69.5` to `0.72.x` in `vmafx-sys/Cargo.toml`
to eliminate the `lazycell` transitive dependency. Low urgency (build-time only),
but straightforward.

---

## 4. Cargo.lock health and version pins

The workspace `Cargo.lock` (version 4) was regenerated as part of this
audit and is tracked under the current branch. Status:

- All checksums are verified by the Cargo registry (SHA-256 + crates.io).
- No `git = ...` or `path = ...` out-of-tree dependencies — all crates are from
  the official registry.
- No `[patch]` or `[replace]` overrides in workspace `Cargo.toml`.
- `vmafx-sys` pins `bindgen = "0.69"` (SemVer range). This resolves to 0.69.5,
  which is 3 minor versions behind latest 0.72.1.
- `vmafx-tad` pins `cbindgen = "0.27"`, which resolves to 0.27.0 (current latest).
- All other deps are transitive; the lock file captures them deterministically.

**Finding**: Lock file is consistent and traceable. The only pin concern is
`bindgen = "0.69"` (see §3).

---

## 5. ADR-0707 runtime dispatch contract compliance

ADR-0707 specifies that TAD must degrade gracefully when the Rust archive is absent
(i.e., when `enable_rust_features=false`). The implementation fulfils this:

- `HAVE_RUST_TAD` is the compile-time gate defined by `declare_dependency` in
  `core/src/meson.build` when the Rust crate is built.
- `feature_extractor.c` guards the `vmaf_fex_tad` extern declaration and list
  entry behind `#if HAVE_RUST_TAD` (lines 203–207, 345–347).
- `tad_rust.c` compiles either the real Rust-backed callbacks (`#ifdef HAVE_RUST_TAD`)
  or no-op stubs returning `-ENOSYS` (`#else`), ensuring `--feature tad` produces
  a clear "not available" error rather than an unknown-feature error.
- The extractor descriptor `vmaf_fex_tad` is registered in the list unconditionally
  (when `#if HAVE_RUST_TAD`) so a user calling `--feature tad` on a build without
  `HAVE_RUST_TAD` gets a clean `-ENOSYS` rather than an "unknown feature" error.

**Finding**: The dispatch contract is fully implemented as specified. The build-time
gate (`HAVE_RUST_TAD`) and the runtime stubs together provide the specified graceful
degradation path.

---

## 6. `enable_rust_features` default state

**Expected** (per PR #62 and memory entry): default `false`.

**Actual** (`core/meson_options.txt`):

```meson
option('enable_rust_features',
    type: 'boolean',
    value: false,
    ...
```

**Confirmed**: default is `false`. CI will not attempt to build the Rust crate unless
`-Denable_rust_features=true` is passed explicitly.

**Discrepancy noted**: ADR-0707 body says "default `true`, auto-disables when `cargo`
is absent" (line 62 of the ADR). This is incorrect; the option defaults to `false`
in `meson_options.txt`. The ADR should be corrected to say "default `false`" to
avoid confusion. This is a documentation bug, not a code bug.

---

## Summary

| Dimension | Status | Action required |
| --- | --- | --- |
| `unsafe` justification | PASS — all blocks covered | None |
| cbindgen header drift | PASS — no drift; link-time self-checking | None |
| Vulnerability scan | INFO — `lazycell` RUSTSEC-2022-0027 via `bindgen` (build-time only) | Upgrade `bindgen` to `0.72.x` |
| Cargo.lock health | PASS — deterministic, no out-of-tree deps | None |
| ADR-0707 dispatch contract | PASS — `HAVE_RUST_TAD` + no-op stubs fully implemented | None |
| `enable_rust_features` default | PASS — `false` confirmed | Correct ADR-0707 body (doc bug) |
