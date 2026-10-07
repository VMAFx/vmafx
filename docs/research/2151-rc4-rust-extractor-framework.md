<!-- markdownlint-disable MD013 MD060 -->
# Research-2151: What the RC4 Rust extractor framework has to match

- **Status**: Active
- **Workstream**: [ADR-1713](../adr/1713-rc4-rust-extractor-framework.md), epic [#1723](https://github.com/VMAFx/vmafx/issues/1723)
- **Last updated**: 2026-10-05

## Question

What must a framework provide so that four Rust feature extractors and the
model prediction of `vmaf_v1.0.16_3d0h` can be written in parallel, run
selectably next to the C extractors, and be proven bit-identical to them,
without changing `libvmaf.h` or the exported symbols of `libvmaf.so`?

## Findings

Measured on master `a6abbffa7`, rustc 1.98.1, host build
`-Denable_rust_features=true`, CPU only.

1. **The TAD pilot does not register.** `feature_extractor.cpp` gates
   `&vmaf_fex_tad` on `HAVE_RUST_TAD`, but that file is compiled in
   `libvmaf_feature_static_lib`, whose dependencies do not include
   `rust_tad_dep`, the only carrier of `-DHAVE_RUST_TAD`. Reproducer:
   `vmaf -r src01_hrc00_576x324.yuv -d src01_hrc01_576x324.yuv -w 576 -h 324
   -p 420 -b 8 --feature tad --no_prediction` prints `problem loading feature
   extractor: tad`. No CI lane builds with the option, so nothing noticed.
2. **A Rust build leaks the Rust standard library from `libvmaf.so`.**
   `nm -D build/src/libvmaf.so` lists hundreds of `_RNv...std...` symbols:
   the static archive's objects keep default visibility, unlike the C objects.
   Every Rust build therefore changes the exported symbol set.
3. **Two Rust static libraries cannot share a link** (each bundles `std`), so
   more crates need one archive. `#[unsafe(no_mangle)]` functions of an rlib
   dependency reach a staticlib through `pub use` with LTO (checked with `nm`
   on a two-crate workspace), so feature crates need not export anything
   themselves.
4. **cbindgen exports constants only from the crate it is run on**, not from
   parsed dependencies; the header is therefore generated from `vmafx-fex`,
   and the one function of the aggregate crate is declared in the
   configuration's trailer.
5. **The C option machinery already does everything a twin needs**: the parser
   applies defaults, aliases, ranges and the model's `%g`-normalised values
   (`dict.cpp`); `vmaf_feature_name_dict_from_provided_features()` decorates
   names from the option table and the priv blob; `reads_prev_prev_ref()`
   reads the priv blob. Copying the C descriptor and keeping a C-layout priv
   blob makes all three work unchanged for the twin.
6. **The JSON receipt names the extractor that ran** (`feature_backends`,
   from `vmaf_registered_feature_extractor()`), which lets a harness prove the
   execution path without a strict mode.
7. **The model option sets** (`model/vmaf_v1.0.16*/*.json`): cambi
   `{high_res_speedup 1080, vis_lum_threshold 0.06, max_val 17}` in all eight
   files; speed_chroma `{nn_floor 0.1, sigma_nn 0.19, weight_var_mode 5,
   max_val 45}` plus `prescale 0.5 / 0.6 bilinear` on `3d0h_2160` / `5d0h`;
   adm `{dlm_weight 0.7, enhn_gain_limit 1, noise_weight 0.02, min_val 0.5,
   csf_mode 2}` with four view-distance / display-height pairs; motion
   `{max_val 18}` plus the five-frame window and moving average on the HFR
   files. The "2160 prescale" is the `speed_chroma` prescale; there is no
   global resize.
8. **The build needs no network only in a workspace without external
   crates.** TAD's only external dependency was the cbindgen build dependency
   for a header nothing included. Removing it is not enough: with an empty
   `CARGO_HOME`, `cargo build --offline --locked -p vmafx-core-rs` in the
   root workspace fails with "failed to select a version" for `bindgen`,
   required by `vmafx-sys`, because cargo resolves the whole workspace even
   for one package. In a workspace of its own (`core/src/rust/Cargo.toml`,
   nine internal crates, no registry entry in its lockfile) the same command
   succeeds from an empty `CARGO_HOME`.

## Hardest bit-exactness points per lane

| Lane | Points |
| --- | --- |
| `speed_chroma` | float GSL-style eigen / QR chain with double steps inside (`alpha`, Givens); glibc `log2` (reference values are glibc-only); bilinear prescale computed in double and narrowed to float; 9- and 14-bit input read as bytes by `picture_copy` |
| `motion` | signed shifts that floor negative values; `prev - cur` order; int32 vs int64 accumulators per bit depth; the five-frame flush (stamp value, `min(sad[i-1], sad[i+1])`), `motion_max_val` applied twice, read-back from the collector |
| `adm` | frozen upstream quirks (`add_flt = INT32_MIN`, `add_sq = 1 << sq`, `65535` rounding); two float restore conventions (double divide at scale 0, float divide at scales 1 to 3); `powf` of float products; `__builtin_clz` normalisation; AIM unclipped; non-finite scores are errors |
| `cambi` | the 4096-entry reciprocal table with 42 literals one ulp off `1.0f/i` (copy, never compute); `lroundf` nearest-neighbour resize with a float accumulator; the bounded quick-select order once a pooled sum exceeds 2^29; mode-filter ties |
| prediction | SV order of the libsvm sum, the sparse merge walk, glibc `exp`, the chroma correction's round-trip denormalisation, the transform's accumulation order |

## Sources

- `core/src/meson.build` (Rust block, `libvmaf_feature_static_lib`),
  `core/src/feature/feature_extractor.{h,cpp}`, `core/src/feature/tad_rust.c`,
  `core/src/feature/rust/tad/`, `core/src/libvmaf.c`
  (`vmaf_use_features_from_model`), `core/tools/cli_feature_backend.cpp`.
- `core/src/feature/{speed.c,integer_motion.c,integer_adm.c,cambi.c}`,
  `core/src/predict.c`, `core/src/read_json_model.cpp`, `core/src/svm.cpp`.
- [cbindgen documentation](https://github.com/mozilla/cbindgen/blob/master/docs.md).
