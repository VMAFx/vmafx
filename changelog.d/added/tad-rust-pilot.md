### TAD (Temporal Absolute Difference) feature extractor — Rust/cbindgen pilot (ADR-0707)

Added the `tad` feature extractor, the first feature in VMAFX implemented in Rust.
The extractor computes the mean absolute difference of luma pixel values between a
reference and distorted frame, normalised to [0.0, 1.0] by the peak luma value.

**Usage:** `--feature tad` (outputs `tad` and `tad_sad` per-frame scores).

**Build:** Requires `cargo` in PATH (detected at configure time). Opt-out via
`-Denable_rust_features=false`.

This PR also establishes the workspace `Cargo.toml` at the repository root and
documents the cbindgen → Meson → `libvmaf.so` integration recipe in ADR-0707 for
future Rust feature extractors to follow.
