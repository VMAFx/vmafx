<!-- markdownlint-disable MD013 MD060 -->
# CAMBI

CAMBI (Contrast Aware Multiscale Banding Index) is Netflix's detector for
banding (aka contouring) artifacts. The score starts at 0, meaning no banding
is detected, and a higher score means more visible banding. Run it with
`--feature cambi`.

## Scores

- **Range.** The score starts at 0. The maximum CAMBI observed in a sequence
  is 24 (unwatchable).
- **Rule of thumb.** A CAMBI score around 5 is where banding starts to become
  slightly annoying. Banding is highly dependent on the viewing environment:
  the brighter the display and the dimmer the ambient light, the more visible
  it is.
- **No reference needed.** By default, the current version of CAMBI is a
  [no-reference metric](https://en.wikipedia.org/wiki/Video_quality#Classification_of_objective_video_quality_models)
  and operates on a frame-by-frame basis (no temporal information is
  leveraged).
- **Full-reference mode.** With `full_ref=true` CAMBI computes its score as
  `MAX(0, distorted_score - reference_score)`, using both inputs (see
  [Options](#options)).

For an introduction to CAMBI, see the
[tech blog](https://netflixtechblog.medium.com/cambi-a-banding-artifact-detector-96777ae12fe2).
For a detailed technical description, see the
[technical paper](../reference/papers/CAMBI_PCS2021.pdf) published at PCS 2021.
The paper describes an initial version of CAMBI that no longer matches the code
exactly, but it is still a good introduction.

## How to run CAMBI

To invoke CAMBI using the VMAF command line, follow the
[instructions](../../core/tools/README.md) and use `cambi` as the feature name.
Because the VMAF framework's API takes a reference and a distorted video, CAMBI
takes both; for the no-reference mode one can point `--reference` and
`--distorted` to the same video path. For example, after downloading the input
video
[`src01_hrc01_576x324.yuv`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/yuv/src01_hrc01_576x324.yuv),
invoke CAMBI via:

```bash
build/tools/vmaf \
    --reference src01_hrc01_576x324.yuv \
    --distorted src01_hrc01_576x324.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature cambi --output /dev/stdout
```

This will yield the output:

```text
<VMAF version="4b42f672">
  <params qualityWidth="576" qualityHeight="324" />
  <fyi fps="52.47" />
  <frames>
    <frame frameNum="0" cambi="0.848047" />
    <frame frameNum="1" cambi="0.723467" />
    ...
    <frame frameNum="46" cambi="0.994815" />
    <frame frameNum="47" cambi="1.019691" />
  </frames>
  <pooled_metrics>
    <metric name="cambi" min="0.509878" max="1.019691" mean="0.689250" harmonic_mean="0.681308" />
  </pooled_metrics>
  <aggregate_metrics />
</VMAF>
```

Options follow the feature name after the first `=`, separated by `:`, for
example `--feature cambi=window_size=50:topk=0.5`.

## Options

| Option | Alias | Type | Default | Range | Effect |
| --- | --- | --- | --- | --- | --- |
| `window_size` | `ws` | int | 65 | 15 to 127 | Window size to compute CAMBI. 65 corresponds to about 1 degree at 4K resolution and 1.5H. |
| `topk` | none | double | 0.6 | 0.0001 to 1.0 | Ratio of pixels for the spatial pooling computation. |
| `cambi_topk` | `ctpk` | double | 0.6 | 0.0001 to 1.0 | Same as `topk`. |
| `tvi_threshold` | `tvit` | double | 0.019 | 0.0001 to 1.0 | Visibility threshold for luminance ΔL < tvi_threshold*L_mean for BT.1886. |
| `cambi_vis_lum_threshold` | `vlt` | double | 0 | 0 to 300 | Luminance value below which banding is assumed not to be visible. |
| `max_log_contrast` | `mlc` | int | 2 | 0 to 5 | Maximum contrast in log luma level (2^max_log_contrast) at 10 bits. The default 2 is equivalent to 4 luma levels at 10-bit and 1 luma level at 8-bit, and is recommended for banding artifacts coming from video compression. |
| `cambi_max_val` | `cmxv` | double | 1000 | 0 to 1000 | Maximum value allowed; larger values are clipped to it. |
| `eotf` | none | string | `bt1886` | `bt1886`, `pq` | EOTF used to compute the visibility thresholds. |
| `cambi_eotf` | `ceot` | string | `bt1886` | `bt1886`, `pq` | Same as `eotf`; takes precedence when both are set. |
| `full_ref` | none | bool | `false` | n/a | Run CAMBI as a full-reference metric: output the per-frame difference between the encoded and source images as well as the existing no-reference score. Not a command-line flag; pass it as `--feature cambi=full_ref=true`. |
| `enc_width`, `enc_height` | `encw`, `ench` | int | 0 | 144 to 7680 (both) | Encoding/processing resolution to compute the banding score, useful when scaling was applied to the input prior to the computation of metrics. |
| `enc_bitdepth` | `encbd` | int | 0 | 6 to 16 | Encoding bit depth. |
| `src_width`, `src_height` | `srcw`, `srch` | int | 0 | 320 to 7680, 200 to 4320 | Encoding/processing resolution of the reference image; only used if `full_ref=true`. |
| `cambi_high_res_speedup` | `hrs` | int | 0 | 1080, 1440, 2160 or 0 | Speed up by downsampling post spatial mask for resolutions at or above the given height. Some loss of accuracy is expected. 0 means not applied. |
| `heatmaps_path` | none | string | unset | n/a | Folder where the heatmaps for different scales are stored as `.gray` files. The path is UTF-8 on every platform, including Windows; non-ASCII parent and leaf directory names are supported. |

### Example: `enc_width` and `enc_height`

The input video
[`KristenAndSara_1280x720_8bit_processed.yuv`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/yuv/KristenAndSara_1280x720_8bit_processed.yuv)
has been encoded at 540p and later upscaled to 720p. Specifying the accurate
encoding width and height lets CAMBI assess the banding artifact more
accurately:

```bash
build/tools/vmaf \
    --reference KristenAndSara_1280x720_8bit_processed.yuv \
    --distorted KristenAndSara_1280x720_8bit_processed.yuv \
    --width 1280 --height 720 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature cambi=enc_width=960:enc_height=540 --output /dev/stdout
```

This will yield the output:

```text
<VMAF version="4b42f672">
  <params qualityWidth="1280" qualityHeight="720" />
  <fyi fps="40000.00" />
  <frames>
    <frame frameNum="0" cambi="1.218365" />
  </frames>
  <pooled_metrics>
    <metric name="cambi" min="1.218365" max="1.218365" mean="1.218365" harmonic_mean="1.218365" />
  </pooled_metrics>
  <aggregate_metrics />
</VMAF>
```

If no encoding width and height parameters are specified (`--feature cambi`),
the output is:

```text
<VMAF version="4b42f672">
  <params qualityWidth="1280" qualityHeight="720" />
  <fyi fps="47619.05" />
  <frames>
    <frame frameNum="0" cambi="0.341833" />
  </frames>
  <pooled_metrics>
    <metric name="cambi" min="0.341833" max="0.341833" mean="0.341833" harmonic_mean="0.341833" />
  </pooled_metrics>
  <aggregate_metrics />
</VMAF>
```

### Full-reference mode

```bash
build/tools/vmaf \
    --reference reference.yuv --distorted distorted.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 10 \
    --no_prediction --feature cambi=full_ref=true --output /dev/stdout
```

Both the `--reference` and `--distorted` inputs are used. There is no
`--full_ref` command-line option (the CLI rejects it as unrecognized).

The run emits three scores: `cambi` (the distorted score), `cambi_source` (the
reference score) and `cambi_full_reference`
(`MAX(0, distorted_score - reference_score)`).

`cambi` is the distorted picture's score at the encoding resolution, the same
value a run without `full_ref` reports: `src_width` and `src_height` only set
the resolution the reference is scored at, so they change `cambi_source` and
`cambi_full_reference`, never `cambi`. For example,
`--feature cambi=full_ref=true:src_width=960:src_height=540` on a 480x270
distorted input upscales the reference to 960x540 for `cambi_source` and
scores the distorted input at 480x270. Before the fix of
`T-CAMBI-10BIT-FULLREF-WIDE-SOURCE-ROWS-2026-10-05`, 10-bit input with a source
larger than the picture scored a distorted picture with shifted rows, so its
`cambi` and `cambi_full_reference` from such runs are wrong; 8-, 9-, 12- and
16-bit input was not affected.

## Inputs

### Bit depths

CAMBI supports the same input bit depths as VMAF: 8, 10, 12 and 16. The
computations in CAMBI are always performed at the 10-bit level, and the other
formats are converted to 10-bit as a preprocessing step.

### Frame sizes

CAMBI needs a width or a height of at least 216 pixels; `init()` refuses a
frame when both are smaller (`CAMBI_MIN_WIDTH_HEIGHT`,
`core/src/feature/cambi_internal.h`).

The window scales with the encode resolution:
`window_size * (width + height) / 6000`, rounded up to an odd number
(`adjust_window_size()` in `core/src/feature/cambi.c`); half of it, rounded
down, is `pad_size`. CAMBI then halves the frame four times, so the coarsest of
its five scales has 1/16 of the rows and columns, and it builds each pixel's
histogram over the window clipped to the frame, as it does at every frame edge.

| Input shape | When it applies (default `window_size` 65) | Effect |
| --- | --- | --- |
| Wide and short | The coarsest scale has no more rows than `pad_size`: every height up to 176 at 1920 wide, 240 at 2560 wide, 352 at 3840 wide. | Scored on the window clipped to the frame. Frames with fewer than `pad_size` rows (up to 160, 224 and 336 rows at those widths) scored differently before 2026-09-30; see [History](#history). |
| Tall and narrow | The coarsest scale has fewer columns than `pad_size`: widths up to 80 at 1080 high, 160 at 1920 high, 176 at 2160 high. | Scored on the window clipped to the frame, on every code path (scalar, SIMD, GPU twins). |
| Very wide | The adjusted window exceeds 65 x 65, the size of the reciprocal table (for example 7680x216). | `init()` fails with `cambi: window_size 85 too large for reciprocal LUT`. A lower `window_size` fits (`cambi=window_size=50` runs at 7680x216). |

[Research-2132](../research/2132-cambi-short-and-narrow-frames.md) defines the
inputs behind the numbers in the history and lists the measurements.

## Generating and Decoding Heatmaps

To generate the heatmaps, run CAMBI with the `heatmaps_path` option set to a
local folder. The CPU extractor and the Metal twin (`integer_cambi_metal`)
write the same files with the same code; the CUDA, SYCL and HIP twins do not
declare the option, so a request or model that sets it keeps the CPU
extractor on those backends. It will write files such as:

```text
cambi_heatmap_scale_0_1280x720_16b.gray
cambi_heatmap_scale_1_640x360_16b.gray
cambi_heatmap_scale_2_320x180_16b.gray
cambi_heatmap_scale_3_160x90_16b.gray
cambi_heatmap_scale_4_80x45_16b.gray
```

containing the raw grayscale heatmap data. You can use `ffmpeg` to convert
them:

```text
ffmpeg -f rawvideo -pix_fmt gray16le -s 1280x720 -i heatmaps/cambi_heatmap_scale_0_1280x720_16b.gray -frames:v 1 heatmaps/cambi_heatmap_scale_0_1280x720_16b.png
```

## Python Library

CAMBI can also be invoked in the [Python library](../usage/python.md). Use
`CambiFeatureExtractor` as the feature extractor and `CambiQualityRunner` as
the quality runner. Use `CambiFullReferenceFeatureExtractor` and
`CambiFullReferenceQualityRunner` to run the full-reference version of CAMBI.

```python
from vmaf.config import VmafConfig
from vmaf.core.asset import Asset
from vmaf.core.cambi_quality_runner import CambiQualityRunner

dis_path = VmafConfig.test_resource_path("yuv", "KristenAndSara_1280x720_8bit_processed.yuv")
asset = Asset(dataset="test", content_id=0, asset_id=0,
              workdir_root=VmafConfig.workdir_path(),
              ref_path=dis_path,
              dis_path=dis_path,
              asset_dict={'width': 1280, 'height': 720,
                          'dis_enc_width': 960, 'dis_enc_height': 540})

qrunner = CambiQualityRunner([asset], None, fifo_mode=False,
                             result_store=None, optional_dict={})
qrunner.run(parallelize=True)

# score: arithmetic mean score over all frames (about 1.218365)
print(qrunner.results[0]['Cambi_score'])
```

## CPU SIMD paths

The CPU extractor picks its kernels once, in `init()`, from the host's CPU
flags. Nothing needs to be configured: the scalar C code is the default and the
reference, and a SIMD kernel replaces it only on a CPU that supports its
instruction set. Every dispatched kernel is bit-exact against the scalar code,
so the CAMBI score is identical whichever path runs.

| Stage | AVX2 | AVX-512 | NEON (aarch64) |
| --- | --- | --- | --- |
| Anti-dithering filter (8-bit input) | dispatched | dispatched | dispatched |
| Spatial mask: derivative row | dispatched | dispatched | dispatched |
| Spatial mask: summed-area (dp) row | dispatched | dispatched | dispatched |
| Spatial mask: box-sum threshold (mask) row | dispatched | dispatched | scalar (see below) |
| Decimate | dispatched | dispatched | dispatched |
| Mode filter | dispatched | dispatched | scalar (see below) |
| c-values (sliding histogram and c-value rows) | dispatched | dispatched | dispatched |

On aarch64 two stages keep the scalar code on purpose. For the mask row and the
mode filter, GCC and Clang already compile the scalar loop into the same NEON
instructions a hand-written kernel uses, so a NEON kernel would remove no work
(measured by instruction count: 0.93x with GCC, 1.02x with Clang for the mode
filter). Both NEON kernels are still built and covered by the parity tests, so
they are ready if a compiler stops vectorising those loops.

### Why the c-values stage is fast

The c-values stage is the expensive one. All three SIMD drivers (AVX2, AVX-512,
NEON) are fast because they skip work, not because their vectors are wider. On
flat, banding-prone content almost every pixel leaves the sliding histogram
unchanged, because it is:

- masked out,
- outside the luma band CAMBI scores, or
- equal to the pixel it replaces.

The scalar driver visits every pixel. The SIMD drivers test a block of up to
256 pixels with vector compares and update the histogram only for the ones that
change it. The histogram each row sees is the same, so the scores are too.
Upstream's AVX2 driver, which visits every pixel, is still built and tested but
no longer used: in icx builds it was slower than the scalar code.

### Compare paths on your machine

Mask CPU flags with `--cpumask`, which takes the flags to *disable*. The score
must not change:

```bash
# default dispatch (AVX-512 or NEON where available)
vmaf -r ref.yuv -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 \
    --no_prediction --feature cambi --precision max --json -o simd.json
# x86: AVX2 only (disable the AVX-512 and AVX-512 ICL flags, bits 16 and 32)
vmaf ... --cpumask 48 -o avx2.json
# x86: scalar only
vmaf ... --cpumask 65535 -o scalar.json
# aarch64: scalar only (disable NEON, bit 0)
vmaf ... --cpumask 1 -o scalar.json
```

The JSON files differ only in `fps`. CAMBI's dispatch tests the AVX-512 flag
only, so `--cpumask 16` gives the same AVX2-only dispatch for CAMBI;
`--cpumask 48` is the form that also disables AVX-512 for every other
extractor.

The tests behind this are in the `simd` suite:

```bash
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- -C build --suite simd
```

- `test_cambi_stage_simd` compares every per-stage kernel with the scalar
  stage.
- `test_cambi_spatial_mask_simd` covers the spatial-mask rows.
- `test_cambi_simd` covers the c-values row.
- `test_cambi_dispatch_invariance` runs the whole extractor at each dispatch
  level and requires identical scores.

### Measured speed

Measured on one AMD Zen 5 core (Ryzen 9 9950X3D), single thread, release
builds, over seven 576x324 to 3840x2176 8- and 10-bit inputs
([Research-2065](../research/2065-cambi-simd-gaps.md)). The c-values stage
against scalar:

| c-values driver | GCC | Clang | icx |
| --- | --- | --- | --- |
| AVX2 | 2.09–2.78x | 3.50–5.52x | 2.87–4.48x |
| AVX-512 | 2.17–3.21x | 4.00–7.08x | 3.04–5.68x |

AVX-512 against AVX2, per stage:

| Stage | GCC | Clang | icx |
| --- | --- | --- | --- |
| Anti-dithering filter | 2.07–2.44x | 1.73–2.03x | 1.23–2.47x |
| Derivative row | 1.31–1.79x | 1.12–1.50x | 1.17–1.45x |
| Decimate | 1.30–1.47x | 1.25–1.32x | 1.35–1.44x |
| Mode filter | 1.37–1.42x | 1.30–1.37x | 1.08–1.18x |
| c-values | 1.04–1.16x | 1.14–1.28x | 1.06–1.27x |

A whole CAMBI frame on that host, single thread, frames per second. "Before" is
the build without these kernels, at its default dispatch; "default" is AVX-512
on this host; "AVX2 only" is `--cpumask 48`, what a CPU without AVX-512 runs:

| Input | GCC, before | GCC, default | GCC, AVX2 only | Clang, AVX2 only | icx, AVX2 only (before) |
| --- | --- | --- | --- | --- | --- |
| 576x324 8-bit | 2024 | 2668 | 2409 | 2540 | 2437 (1388) |
| 1920x1088 8-bit | 191 | 252 | 229 | 240 | 227 (128) |
| 1920x1080 10-bit | 267 | 339 | 313 | 338 | 318 (180) |
| 3840x2176 8-bit | 46.8 | 60.5 | 56.4 | 57.4 | 56.7 (33.1) |

On aarch64 there was no hardware to time; under `qemu-aarch64` the NEON kernels
execute 9–24 % of the scalar instructions for the anti-dithering filter,
derivative row and decimate, and 21–44 % for the c-values stage.

The spatial-mask row kernels, measured on their own (1920x1080 frame, 7x7 mask
filter, [Research-2062](../research/2062-cambi-spatial-mask-simd.md)):

| Kernel | Scalar | AVX2 | AVX-512 |
| --- | --- | --- | --- |
| dp row, GCC build | 486 µs | 216 µs (2.25x) | 153 µs (3.18x) |
| dp row, Clang build | 428 µs | 236 µs (1.81x) | 165 µs (2.59x) |
| mask row, GCC build | 275 µs | 164 µs (1.68x) | 122 µs (2.26x) |
| mask row, Clang build | 251 µs | 167 µs (1.51x) | 121 µs (2.08x) |

## Rust twin

A build with `-Denable_rust_features=true` also registers `cambi_rust`, a Rust
port of the CPU extractor (RC4, [ADR-1713](../adr/1713-rc4-rust-extractor-framework.md)).
It follows the scalar C path of `core/src/feature/cambi.c` statement by
statement and returns the C extractor's scores bit for bit. The C extractor
stays the default.

```bash
# Name the twin: always runs the Rust code.
build/tools/vmaf --reference ref.yuv --distorted dis.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --feature cambi_rust --no_prediction --output /dev/stdout --json

# Or switch every extractor that has a Rust twin, models included.
VMAF_FEATURE_IMPL=rust build/tools/vmaf ... --feature cambi
```

The JSON `feature_backends` array names `cambi_rust` when the Rust code ran.
How the Rust path is built and selected, and how the fallback to C works, is in
[the Rust extractor framework guide](../development/rust-extractor-framework.md#build-and-run-the-rust-path).
The twin reads the same option table as `cambi` (names, aliases, defaults and
ranges), so the [Options](#options) above apply unchanged and the emitted
feature names are the same. Every option is implemented except
`heatmaps_path`: the twin refuses it at init with "cambi_rust: heatmaps_path is
not implemented; use the C extractor".

Per-frame scores equal the C extractor's at `--precision max`, compared as
IEEE doubles, on every fixture and option set the RC4 harness derives
(`scripts/ci/rust_twin_diff.py --feature cambi`): the default options and the
options every `vmaf_v1.0.16` model sets
(`cambi_high_res_speedup=1080:cambi_vis_lum_threshold=0.06:cambi_max_val=17`).

| Fixture | Frames | Default options | `vmaf_v1.0.16` options |
| --- | --- | --- | --- |
| Netflix `src01_hrc00` / `src01_hrc01`, 576x324 8-bit | 48 | 48 of 48 identical | 48 of 48 identical |
| Checkerboard 1-px, 1920x1080 8-bit | 3 | 3 of 3 identical | 3 of 3 identical |
| Checkerboard 10-px, 1920x1080 8-bit | 3 | 3 of 3 identical | 3 of 3 identical |
| Sparks, 480x270 10-bit | 5 | 5 of 5 identical | 5 of 5 identical |
| BBB, 3840x2160 8-bit | 200 | 200 of 200 identical | 200 of 200 identical |

A wider sweep on the Netflix and sparks pairs (`max_log_contrast` 0 to 5,
`enc_width` / `enc_height` resizing, `enc_bitdepth`, `full_ref` with and
without a resized source, `topk`, `cambi_topk`, `tvi_threshold`, both EOTFs,
`window_size` 15 to 127, `cambi_vis_lum_threshold` up to 20, `cambi_max_val`)
and on the 1080p and 4K pairs (every `cambi_high_res_speedup` tier, resizing,
`full_ref`) was identical on all 68 cells. With
`cambi_vis_lum_threshold=300` both refuse init (empty value band).

One configuration differs, because of a defect in the C extractor:
`full_ref=true` with 10-bit input and `src_width` / `src_height` larger than
the picture. There the C copies the distorted plane with the input's stride
into a wider buffer and shifts its rows
(`T-CAMBI-10BIT-FULLREF-WIDE-SOURCE-ROWS-2026-10-05` in
[the state ledger](../state.md)); the twin copies row by row, which is what the
C will do once that row is fixed.

The twin is the scalar C path in Rust, so it is slower than the C extractor's
SIMD dispatch. CPU milliseconds per frame (user plus system time of the `vmaf`
process, `(t(N) - t(2)) / (N - 2)`, median of 3, one extraction thread; Ryzen 9
9950X3D, GCC 16 build with `-Denable_rust_features=true`, host under load):

| Fixture | Options | C, default dispatch (AVX-512) | C, scalar (`--cpumask 65535`) | `cambi_rust` |
| --- | --- | --- | --- | --- |
| 576x324, 48 frames | default | 0.95 | 1.72 | 2.63 |
| 576x324, 48 frames | `vmaf_v1.0.16` | 0.93 | 1.71 | 2.67 |
| 3840x2160, 50 frames | default | 47.64 | 94.26 | 139.02 |
| 3840x2160, 50 frames | `vmaf_v1.0.16` | 29.48 | 54.20 | 63.29 |

Speed is not part of the RC4 contract; tuning belongs to the benchmark
candidate.

`core/test/test_rust_cambi_kernels.c` (Meson suite `rust`) compares the
stages the fixtures reach only for a few option values with the C functions
value by value: all 4226 entries of the reciprocal table, the TVI and
visibility tables for both EOTFs, every `max_log_contrast` and a sweep of
thresholds, the adjusted window and spatial-mask index over 361 frame sizes,
and the resize walk.

## GPU twins

CAMBI has CUDA, SYCL and HIP twins that compute every stage on the device, and
a Metal twin that is a hybrid. Every twin reproduces the CPU's score; the exact
ones are declared in `scripts/ci/exact_twins.d/`. The Vulkan backend (formerly
T7-36 / [ADR-0210](../adr/0210-cambi-vulkan-integration.md)) was removed in
[ADR-0726](../adr/0726-drop-vulkan-backend.md); CAMBI has no Vulkan path.

| Twin | Invocation | What runs on the device | Exact vs CPU | ADR | Evidence (fragment) |
| --- | --- | --- | --- | --- | --- |
| `cambi_cuda` | `--backend cuda` | Whole frame; one 88-byte readback per frame | Yes, while `cambi.c`'s own top-K `double` sum is exact | [ADR-1379](../adr/1379-cuda-cambi-device-resident-pipeline.md), [ADR-0360](../adr/0360-cambi-cuda.md) | `cambi.cuda`: 178 of 178 frames and 200 BBB 4K frames on an RTX 4090 |
| `cambi_sycl` | `--backend sycl` | Whole frame; one 88-byte readback per frame | Yes, while the CPU's top-K `double` sum is exact | [ADR-1357](../adr/1357-sycl-cambi-device-resident.md) | `cambi.sycl`: 333 of 333 frames on an Arc A380 |
| `cambi_hip` | `--backend hip` | Whole frame; one 88-byte readback per frame | Yes, while the CPU's top-K `double` sum is exact | [ADR-1378](../adr/1378-hip-cambi-device-resident.md) | `cambi.hip`: 178 of 178 frames on a gfx1036 |
| `integer_cambi_metal` | `--backend metal` | Spatial mask, 2x decimate, 3-tap separable mode filter; the sliding-histogram `calculate_c_values` and the top-K spatial pool run on the host after a device-to-host copy at every scale | No (gate `places=4`) | none | none |

!!! warning "Re-measure any CAMBI score taken on the HIP backend before ADR-1219"
    Up to and including v3.2.1 the HIP twin returned exactly `0.0` on banding
    content that the CPU scores at `5.85`. It is fixed and now bit-exact with
    the CPU ([ADR-1219](../adr/1219-gpu-cambi-tvi-shared-bisection.md); details
    under [History](#history)). The Metal twin carried part of the defect and
    gets the same fixes; CUDA and SYCL were unaffected.

With `--backend cuda|sycl|hip`, `--feature cambi` runs the twin too
([ADR-1359](../adr/1359-cli-feature-backend-twin.md)): the JSON
`feature_backends` array names the extractor that ran, and a model that lists
`cambi` (the default model does) picks the twin on its own. Naming the twin
(`--feature cambi_cuda`) always registers it. For HIP, `cambi_hip` declares the
CPU extractor's provided feature, so the pairing is the same; it was not
checked on a device for this page.

Every device twin shares the CPU's window guard: when the adjusted window
exceeds 65 x 65 (the size of the reciprocal table), `init()` fails with
"cambi: window_size N too large for reciprocal LUT". At 3840x2160 that is any
`window_size` above 65 without `cambi_high_res_speedup`. Before ADR-1379 the
CUDA twin had no such check.

### Speed

Milliseconds per frame, `(t(N) - t(2)) / (N - 2)`, median of 3 (CUDA, before
and after ADR-1379 in alternation on the same host;
[Research-1379](../research/1379-cuda-cambi-speed-device-resident.md) has the
method and the raw numbers):

| Twin and device | Size | Before | After | CPU, 16 threads |
| --- | --- | ---: | ---: | ---: |
| `cambi_cuda`, RTX 4090 | 3840x2160 | 64.71 | 6.01 | 19.65 |
| `cambi_cuda`, RTX 4090 | 576x324 | 1.59 | 0.38 | 0.09 |

For SYCL, measured on Big Buck Bunny, 3840x2160 8-bit 4:2:0, `--precision max`,
from `t(22 frames) - t(2 frames)`, median of five to nine runs
(ADR-1357, "before" is the host-residual implementation):

| Device | Before | After |
| --- | ---: | ---: |
| CPU, `--backend cpu --threads 16 --feature cambi` (unchanged code) | 10.6 | 11.6 |
| Intel Arc B580, `--feature cambi_sycl` | 140 | 9.3 |
| Intel UHD 770, `--feature cambi_sycl` | 944 | 42 |
| Intel UHD 770, default model | 976 | 103 |
| Intel Arc B580, default model | 123 | 71 |

The HIP twin has not been timed on AMD hardware; the parity and timing
commands are in [`docs/state.md`](../state.md) under
`T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29`. Re-check parity and timing on your own
device with
`python3 scripts/dev/speed_gpu_parity.py --backend cuda --feature cambi --vmaf $PWD/build-cuda/tools/vmaf`
(the SYCL commands are in
[Research-2122](../research/2122-sycl-cambi-device-resident.md)).

### CUDA

```bash
# Build with CUDA enabled
meson setup build-cuda core -Denable_cuda=true
ninja -C build-cuda

# Run with --backend cuda
./build-cuda/tools/vmaf -r ref.yuv -d dis.yuv -w W -h H \
    -p 420 -b 8 --backend cuda --feature cambi_cuda
```

`cambi_cuda` ([ADR-1379](../adr/1379-cuda-cambi-device-resident-pipeline.md))
is the design of the SYCL twin on CUDA. It reads the distorted plane the CUDA
engine already uploaded, so it adds no transfer of its own, and it waits once
per frame, in `collect()`, so frames overlap with the other CUDA extractors.

The arithmetic is the SYCL twin's:

- The c-values keep `cambi.c`'s column histograms and reciprocal table.
- Top-K pooling sums the largest `topk` fraction exactly as an integer in units
  of $2^{-24}$.
- The score therefore equals `--backend cpu` to the last bit whenever the CPU's
  own `double` sum of those values is exact, and otherwise differs by that
  sum's rounding. On a synthetic, heavily banded 3840x2160 clip that was at
  most 3.0e-13, and a CPU build that sums in `long double` matched the CUDA
  twin exactly
  ([Research-1379](../research/1379-cuda-cambi-speed-device-resident.md)).

Measured on an RTX 4090 against an icx build of the CPU extractor, at
`--precision max`: every per-frame `cambi` is identical to `--backend cpu` on
the Netflix 576x324 pair (48 frames) and BBB 3840x2160 (50 frames). On the
wide, short frames (1920x64 to 3840x128), where the CPU extractor needs the
`T-CAMBI-SHORT-FRAME-OOB-2026-09-30` fix, the twin equals the fixed CPU with
`compute-sanitizer` clean.

Companion research digest:
[Research-0091](../research/0091-cambi-cuda-integration.md) (CUDA).

### SYCL

`cambi_sycl` ([ADR-1357](../adr/1357-sycl-cambi-device-resident.md)) takes the
distorted plane from the SYCL frame upload every SYCL extractor shares, so it
adds no transfer of its own, and it joins the combined command graph with the
other SYCL extractors.

```bash
meson setup build-sycl core -Denable_sycl=true
ninja -C build-sycl

# --feature cambi with --backend sycl runs cambi_sycl too (ADR-1359). A model
# that lists cambi (the default model does) picks cambi_sycl on its own.
./build-sycl/tools/vmaf -r ref.yuv -d dis.yuv -w W -h H \
    -p 420 -b 8 --backend sycl --feature cambi_sycl
```

How the device matches `cambi.c`:

- **c-values.** Each work-item keeps one column of the level histogram for a
  band of rows and slides the window down, as `calculate_c_values` does. The
  histogram cells always hold the true count of their level in the window, so
  the counts equal the CPU's, and each c-value is `c_value_pixel()`'s formula
  with the same reciprocal table (`vmaf_cambi_reciprocal_lut()`).
- **Top-K pooling.** `cambi.c` averages the largest `topk` fraction of c-values
  after a quick-select, summing in `double`. The device finds the same set with
  a radix select and sums it exactly, as an integer in units of $2^{-24}$ (every
  non-zero c-value is at least 0.5 and below $2^{14}$).
- **Agreement.** The two agree to the last bit whenever the CPU's own sum is
  exact. On frames with a very large banded area the CPU's sum rounds and the
  scores differ in the last few digits: at most 2.2e-15 over 50 frames of Big
  Buck Bunny at 3840x2160 (47 identical), against a `places=4` gate.

### HIP

`cambi_hip` ([ADR-1378](../adr/1378-hip-cambi-device-resident.md)) runs the
same pipeline as `cambi_sycl`:

- Per frame it copies the distorted luma into pinned memory and uploads it
  without waiting.
- It runs every stage on the extractor's stream and reads back one 88-byte
  block of exact per-scale sums; `collect()` is the only wait.
- The window, mask index, resize tables, contrast weights, reciprocal table,
  top-K mean and the window guard are `cambi.c`'s own helpers, so the twin
  accepts and rejects the same configurations as the CPU (a window above
  65 x 65 fails init with the same message) and is expected to score
  bit-identically wherever the CPU's top-K sum is exact.

```bash
# -Dhip_gfx_targets: your device's target, as rocm_agent_enumerator prints it
meson setup build-hip core -Denable_hip=true -Denable_hipcc=true \
    -Dhip_gfx_targets=gfx1036
ninja -C build-hip

./build-hip/tools/vmaf -r ref.yuv -d dis.yuv -w W -h H \
    -p 420 -b 8 --backend hip --feature cambi_hip
```

The kernels' arithmetic lives in one header that a host test replays kernel by
kernel against `cambi.c` (`test_hip_cambi_device_math`, no AMD device needed).

### Metal

The Metal twin uses the Strategy II hybrid architecture: the integer phases
(spatial mask, 2x decimate, 3-tap separable mode filter) run on the GPU; the
sliding-histogram `calculate_c_values` and the top-K spatial pool run on the
host after a device-to-host copy at every scale. The cross-backend gate runs at
`places=4`.

## History

### 2026-09-30: short and narrow frames

[ADR-1393](../adr/1393-cambi-clip-window-to-frame.md).

**Wide, short input.** Up to 2026-09-30 the c-values pass read and wrote rows
past both ends of such frames and could crash
([Netflix/vmaf#1628](https://github.com/Netflix/vmaf/issues/1628); the upstream
fix is [Netflix/vmaf#1629](https://github.com/Netflix/vmaf/pull/1629)). It now
stops at the frame.

Frames with fewer than `pad_size` rows at the coarsest scale score differently
where they completed before: on 3-frame 8-bit ramps with `--cpumask 63`,
3840x128 moves from 19.544347 to 19.512269, 1920x160 from 22.270340 to
22.267602 and 3840x256 from 21.778393 to 21.769946. At exactly `pad_size` rows
only a row outside the frame was written, so those frames (1920x176, 2560x240,
3840x352) score as before, as does every frame with more rows.

**Tall, narrow input.** Up to 2026-09-30 the scalar c-values walk read the
columns past such a frame, pixels a finer scale left in the picture stride,
while the SIMD paths did not. `--cpumask 63` builds and the GPU twins that ran
the scalar walk on the host (CUDA, HIP and Metal at the time) could therefore
score such frames differently from the default dispatch.

Measured: a 64x1920 vertical ramp scored 14.975700714938673 on master against
14.964394451743877 on the branch on the C path (the SIMD paths already agreed);
another vertical ramp variant gave 16.141046 on the C path and 16.131541 with
SIMD.

Every path now gives the SIMD result, which is the score of the window clipped
to the frame, so the C path's and those twins' scores of such frames change (the
64x1920 ramp to 14.964394451743877 and 16.131541, a 128x1920 one from
16.204770 to 16.201631).

**Scope.** The column bound goes beyond the upstream fix, which bounds only the
rows; upstream's C path still reads those columns.

### The HIP twin before ADR-1219

Up to and including v3.2.1 the HIP CAMBI twin returned exactly `0.0` on banding
content that the CPU scores at `5.85`.

- It hand-rolled the TVI-threshold bisection over an inverted predicate seeded
  from luma 0 instead of `luma_range.foot`, producing
  `tvi_for_diff = [1026, 1025, 1024, 4]` where the CPU produces
  `[182, 309, 436, 563]`. That collapses the scored luma band from 564 entries
  to a handful, and `calculate_c_values()` then discards almost every pixel as
  out-of-band.
- `filter_mode` filtered output rows 0 and `height-1`, which `cambi.c` leaves
  unfiltered.
- The 7x7 mask box sum clamped out-of-frame taps to the border pixel where the
  CPU's summed-area table zero-pads them.

All three are fixed per
[ADR-1219](../adr/1219-gpu-cambi-tvi-shared-bisection.md) and HIP is now
bit-exact with the CPU. The Metal twin carried the first two and gets the same
fixes; CUDA and SYCL were unaffected.

### The CUDA twin before ADR-1379

**Implementation note.** The twin downloaded the distorted picture to a host
copy and preprocessed it there, because the host preprocessing path reads
`pic->data[0]` as a host
pointer and a CUDA picture holds a device address in that field
(lusoris/vmaf#870); it then read the image and mask back at every scale for the
host c-values and pooling ([ADR-0360](../adr/0360-cambi-cuda.md)).
