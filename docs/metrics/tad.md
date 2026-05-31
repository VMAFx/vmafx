<!-- markdownlint-disable MD013 MD060 -->
# TAD — Temporal Absolute Difference

**Feature name (CLI):** `tad`
**Output scores:** `tad`, `tad_sad`
**Implementation:** Rust (cbindgen pilot — ADR-0707)
**Availability:** CPU only. Requires `enable_rust_features=true` (default when `cargo` is in PATH).

---

## What it measures

TAD is the mean absolute difference of luma (Y) pixel values between a reference frame
and a distorted frame, normalised to the [0.0, 1.0] range by the peak luma value
(2^bpc − 1):

```text
tad(ref, dis) = (1 / (W × H × peak)) × Σ_{i,j} |ref_Y[i,j] − dis_Y[i,j]|
```

where:

- `W × H` = number of luma pixels per frame
- `peak` = maximum luma sample value (255 for 8-bit, 1023 for 10-bit, etc.)
- Only the luma (Y) plane is used; chroma planes are ignored.

A second sub-score `tad_sad` reports the raw (unnormalised) sum of absolute differences,
useful for debugging without the normalisation step.

---

## Score range and interpretation

| TAD value | Meaning |
|-----------|---------|
| 0.0 | Reference and distorted frames are pixel-identical. |
| 0.0 – 0.05 | Very small per-pixel error; typically high-quality encoding. |
| 0.05 – 0.2 | Moderate per-pixel error; perceptual quality may still be good depending on spatial distribution. |
| 0.2 – 1.0 | Large per-pixel error; indicates significant distortion. |
| 1.0 | Maximum possible mean error (every pixel at the opposite extreme). |

**Important:** TAD measures raw pixel fidelity, not perceptual quality. A low TAD score
is a necessary but not sufficient condition for high perceptual quality. Spatially
structured errors (blocking, ringing, blur) can produce low mean errors while being
visually objectionable. Use TAD as a lightweight pre-filter or diagnostic tool, not
as a substitute for VMAF.

---

## Usage

```bash
# Score a single reference/distorted pair and include TAD:
vmaf --reference ref.yuv --distorted dist.yuv \
     --width 1920 --height 1080 --pixel_format yuv420p --bitdepth 8 \
     --feature tad

# TAD alongside the default VMAF model:
vmaf --reference ref.yuv --distorted dist.yuv \
     --width 1920 --height 1080 --pixel_format yuv420p --bitdepth 8 \
     --model version=vmaf_v0.6.1 --feature tad
```

The output JSON will contain per-frame `tad` and `tad_sad` scores, and their
aggregate (mean, harmonic mean, etc.) in the summary block.

---

## Build requirements

TAD is implemented in Rust. The Meson build option `enable_rust_features` (default `true`)
controls whether the Rust crate is compiled. When `cargo` is not found at configure time,
the option is automatically set to `false` and the TAD extractor is silently omitted.

```bash
# Explicit opt-in (default):
meson setup build -Denable_rust_features=true

# Opt-out (e.g. on hosts without a Rust toolchain):
meson setup build -Denable_rust_features=false
```

When `enable_rust_features=false`, requesting `--feature tad` at runtime returns
`-ENOSYS` (feature not available).

---

## Caveats and limitations

- **Luma only.** The metric intentionally ignores chroma, matching the convention
  of most VMAF component features which also focus on the luma plane. A future
  `tad_chroma` variant could be added.
- **No temporal context.** TAD is computed independently per frame pair; it does not
  accumulate across frames and has no temporal memory.
- **Not part of any VMAF model.** TAD scores do not affect VMAF scores; it is an
  additive signal only.

---

## Implementation notes

Source files:

- `core/src/feature/rust/tad/src/lib.rs` — Rust implementation + unit tests
- `core/src/feature/rust/tad/Cargo.toml` — crate manifest (cbindgen build-dep)
- `core/src/feature/rust/tad/build.rs` — cbindgen header generation
- `core/src/feature/tad_rust.c` — C wrapper adapting the Rust ABI to `VmafFeatureExtractor`

Architecture: ADR-0707 documents the cbindgen → Meson integration recipe. Future Rust
feature extractors should follow the same pattern.
