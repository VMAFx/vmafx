<!-- markdownlint-disable MD013 MD060 -->
# ADR-0707: TAD — Temporal Absolute Difference Feature Extractor Implemented in Rust (cbindgen Pilot)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `rust`, `build`, `metrics`, `feature-extractor`, `phase4`, `cbindgen`, `fork-local`

## Context

Phase 4 of the VMAFX modernization plan calls for incrementally introducing Rust
into the libvmaf codebase to prove the integration story end-to-end before committing
to larger rewrites. The specific goal of this ADR is to pick one new feature extractor,
implement it in Rust, expose it to C via `cbindgen`-generated bindings, and wire it
into the Meson build so the result links into `libvmaf.so`.

The selected metric is **TAD — Temporal Absolute Difference**: the mean absolute
difference of luma pixel values between a reference and a distorted frame, normalised
by the peak luma value (2^bpc − 1) to the range [0.0, 1.0].

```text
tad(ref, dis) = (1 / (W × H × peak)) × Σ |ref_y[i,j] − dis_y[i,j]|
```

TAD was chosen because:

- It has no existing C implementation in this codebase, so there is no bit-exactness
  ambiguity or risk of breaking the Netflix golden-data gate.
- The computation is numerically trivial (sum-of-absolute-differences) and fully
  specified by a single formula, making the Rust implementation easy to review and test.
- It is genuinely useful as a standalone signal: a near-zero TAD (low mean error) is
  a necessary but not sufficient condition for high perceptual quality, so it can serve
  as a quick sanity-check filter upstream of heavier metrics.
- The implementation fits in approximately 200 lines of Rust, well within the
  "one PR" scope requirement.

The metric is **off by default** — it is only computed when `--feature tad` is
explicitly requested. It does not participate in any VMAF model and does not affect
existing VMAF scores.

## Decision

We will implement TAD as a Rust `staticlib` crate at
`core/src/feature/rust/tad/`, use `cbindgen` to generate a C header, and expose
three C-ABI lifecycle functions (`vmafx_tad_init`, `vmafx_tad_extract`,
`vmafx_tad_close`). A thin C wrapper (`tad_rust.c`) adapts these into a
`VmafFeatureExtractor` descriptor registered in `feature_extractor_list[]` under
the name `"tad"`.

Integration architecture:

1. A workspace `Cargo.toml` at the repository root declares all future Rust crates.
2. A Meson `custom_target` in `core/src/meson.build` runs `cargo build --release`
   and copies the output `libvmafx_tad.a` to the build directory.
3. A `declare_dependency` wraps the archive with `-DHAVE_RUST_TAD` (compile flag)
   and the archive path (link flag). This dependency is attached to the `libvmaf`
   `library()` target only — not to the intermediate static libs — to avoid
   unresolved-symbol failures in test binaries that link against `libvmaf.a` without
   carrying the Rust archive.
4. `tad_rust.c` is compiled as a direct source of the `libvmaf` library target
   (not into `libvmaf_feature.a`) for the same isolation reason.
5. `feature_extractor.c` gates the `vmaf_fex_tad` extern and list entry behind
   `#if HAVE_RUST_TAD`, so static-lib-linked tests compile cleanly without the archive.
6. A Meson option `enable_rust_features` (default `false`; set to `true` to enable
   Rust builds — auto-skips with a warning if `cargo` is absent) lets operators opt
   into the Rust build. CI defaults to `false`; environments with Rust toolchains
   should pass `-Denable_rust_features=true` explicitly.
   (Note: an earlier draft of this ADR incorrectly stated the default as `true`.
   The implemented default is `false`, confirmed in `core/meson_options.txt`.
   Corrected by Research-0760.)

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Implement as plain C (no Rust) | Zero toolchain risk; instant integration | Does not prove the Rust-in-libvmaf story | The explicit goal is to prove the Rust integration path |
| Choose an existing metric (e.g. rewrite PSNR in Rust) | Validates bit-exactness | Bit-exactness across C/Rust is finicky; risk of breaking Netflix golden gate | New feature preferred to avoid ambiguity |
| Use `blockiness` / blur indicator as the pilot metric | More perceptually meaningful | Significantly more complex (~250 lines); fits less cleanly in one PR | TAD's simplicity makes the cbindgen story clearer |
| Use `frame_brightness_p10_p90` luma statistics | Trivially simple; ~50 lines | Too trivial — does not exercise all three ABI lifecycle hooks properly | TAD exercises init + extract (with real pixel work) + close |
| FFI via C++ instead of pure C | Already in use (libsvm, pdjson) | More complex linking; Rust → C++ ABI is less stable than Rust → C | Pure C ABI is the most portable and future-proof surface |
| Dynamic linking of the Rust crate | Separate .so deployable | Complicates package management; libvmaf.so already bundles everything | Static archive (`staticlib`) keeps libvmaf.so self-contained |

## Consequences

- **Positive**:
  - Establishes the cbindgen → Meson integration recipe that future Rust feature
    extractors can follow (see `docs/development/rust-feature-guide.md`).
  - The workspace `Cargo.toml` at the repo root is now the entry point for all future
    Rust crates; adding a new one requires only appending a `members` entry.
  - `enable_rust_features=false` provides a clean escape hatch for CI environments
    without a Rust toolchain.
  - TAD is available to users as `--feature tad` immediately; the score is on a
    well-understood [0, 1] scale with clear semantics.

- **Negative**:
  - Adds `cargo` as an optional build-time dependency. CI matrices without Rust
    must set `-Denable_rust_features=false` or install cargo.
  - LTO interaction: `tad_rust.c` must be compiled as a direct source of the
    `libvmaf` library target rather than into the intermediate `libvmaf_feature.a`,
    because static-lib-linked test executables do not carry `rust_tad_dep` and would
    fail to link otherwise. This is a minor structural asymmetry documented here for
    future Rust feature authors to follow.

- **Neutral / follow-ups**:
  - ADR-0708 (planned): `vmafx-sys` Rust bindings for the public C API — companion
    crate, different purpose from this pilot.
  - The `enable_rust_features` option should be wired into CI job matrices.
  - Long-term: if more than 2–3 Rust extractors land, reconsider whether the
    `HAVE_RUST_TAD` per-extractor define pattern should be replaced by a single
    `HAVE_RUST_FEATURES` umbrella or a linker-script approach.

## References

- Phase 4 VMAFX modernization plan (user direction, 2026-05-28): paraphrased as
  "Rust pilot: one new feature extractor implemented in Rust, exposed to libvmaf
  via C ABI through cbindgen. Proves the Rust-in-libvmaf integration story end-to-end."
- [ADR-0700](0700-vmafx-repo-layout.md) — repo layout rename (`libvmaf/` → `core/`).
- [cbindgen documentation](https://github.com/mozilla/cbindgen)
- Related PR: `feat/tad-rust-pilot`
