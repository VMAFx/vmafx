<!-- markdownlint-disable MD013 -->
# CAMBI

CAMBI (Contrast Aware Multiscale Banding Index) is Netflix's detector for banding (aka contouring) artifacts.

## Background

For an introduction to CAMBI, please refer to the [tech blog](https://netflixtechblog.medium.com/cambi-a-banding-artifact-detector-96777ae12fe2). For a detailed technical description, please refer to the [technical paper](../reference/papers/CAMBI_PCS2021.pdf) published at PCS 2021. Note that the paper describes an initial version of CAMBI that no longer matches the code exactly, but it is still a good introduction.

By default, the current version of CAMBI is a [no-reference metric](https://en.wikipedia.org/wiki/Video_quality#Classification_of_objective_video_quality_models), and operates on a frame-by-frame basis (no temporal information is leveraged). To integrate it as part of the VMAF framework, which employs a [full-reference metric](https://en.wikipedia.org/wiki/Video_quality#Classification_of_objective_video_quality_models) API, CAMBI takes both a reference and a distorted video as its input. For simplicity, one can point the input arguments `--reference` and `--distorted` to the same video path.

CAMBI also offers a full-reference mode which computes its score as `MAX(0, distorted_score - reference_score)`. This mode can be activated with the `--full_ref` command line option. In this case, both the `--reference` and `--distorted` inputs will be used.

## Scores

The CAMBI score starts at 0, meaning no banding is detected. A higher CAMBI score means more visible banding artifacts are identified. The maximum CAMBI observed in a sequence is 24 (unwatchable). As a rule of thumb, a CAMBI score around 5 is where banding starts to become slightly annoying (also note that banding is highly dependent on the viewing environment - the brighter the display, and the dimmer the ambient light, the more visible banding is).

## How to run CAMBI

To invoke CAMBI using the VMAF command line, follow the [instruction](../../core/tools/README.md) and use `cambi` as the feature name. For example, after downloading the input video [`src01_hrc01_576x324.yuv`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/yuv/src01_hrc01_576x324.yuv), invoke CAMBI via:

```bash
core/build/tools/vmaf \
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

## Bit depths

CAMBI supports the same input bit depths as VMAF: 8, 10, 12 and 16. However, the computations in CAMBI will always be performed at the 10-bit level, and the other formats will be converted to 10-bit as a preprocessing step.

## Options

The CAMBI feature extractor also supports additional optional parameters as listed below:

- `window_size` (min: 15, max: 127, default: 63): Window size to compute CAMBI (default: 63 corresponds to ~1 degree at 4K resolution and 1.5H)
- `topk` (min: 0, max: 1.0, default: 0.6): Ratio of pixels for the spatial pooling computation
- `tvi_threshold` (min: 0.0001, max: 1.0, default: 0.019): Visibility threshold for luminance ΔL < tvi_threshold*L_mean for BT.1886
- `max_log_contrast` (min: 0, max: 5, default: 2): Maximum contrast in log luma level (2^max_log_contrast) at 10-bits. Default 2 is equivalent to 4 luma levels at 10-bit and 1 luma level at 8-bit. The default is recommended for banding artifacts coming from video compression.
- `full_ref`: optional flag (default: false) to run CAMBI as a full-reference metric, outputting the per-frame difference between the encoded and source images as well as the existing no-reference score.
- `enc_width` and `enc_height`: Encoding/processing resolution to compute the banding score, useful in cases where scaling was applied to the input prior to the computation of metrics
- `src_width` and `src_height`: Encoding/processing resolution to compute the banding score on the reference image, only used if `full_ref=true`.
- `cambi_high_res_speedup` to speed up by downsampling post spatial mask for resolutions >= 1080p. However, some loss of accuracy is expected in metric. Possible min resolutions for the speed-up = [1080, 1440, 3840, 0]. Default: 0 (not applied).
- `heatmaps_path`: Set to a folder where the heatmaps for different scales will be stored as `.gray` files. The path is UTF-8 on every platform, including Windows; non-ASCII parent and leaf directory names are supported.

An example using the `enc_width` and `enc_height` options on the input video [`KristenAndSara_1280x720_8bit_processed.yuv`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/yuv/KristenAndSara_1280x720_8bit_processed.yuv) which has been encoded at 540p and later upscaled to 1280p (specifying the accurate encoding width and height as input allows CAMBI to more accurately assess the banding artifact):

```bash
core/build/tools/vmaf \
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

If no encoding width and height parameters are specified:

```bash
core/build/tools/vmaf \
    --reference KristenAndSara_1280x720_8bit_processed.yuv \
    --distorted KristenAndSara_1280x720_8bit_processed.yuv \
    --width 1280 --height 720 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature cambi --output /dev/stdout
```

The output will be:

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

## Generating and Decoding Heatmaps

To generate the heatmaps, run CAMBI with the `heatmaps_path` option set to a local folder. It will write files such as:

```text
cambi_heatmap_scale_0_1280x720_16b.gray
cambi_heatmap_scale_1_640x360_16b.gray
cambi_heatmap_scale_2_320x180_16b.gray
cambi_heatmap_scale_3_160x90_16b.gray
cambi_heatmap_scale_4_80x45_16b.gray
```

containing the raw grayscale heatmap data.

You can use `ffmpeg` to convert them:

```text
ffmpeg -f rawvideo -pix_fmt gray16le -s 1280x720 -i heatmaps/cambi_heatmap_scale_0_1280x720_16b.gray -frames:v 1 heatmaps/cambi_heatmap_scale_0_1280x720_16b.png
```

## Python Library

CAMBI can also be invoked in the [Python library](../usage/python.md). Use `CambiFeatureExtractor` as the feature extractor, and `CambiQualityRunner` as the quality runner. Use `CambiFullReferenceFeatureExtractor` and `CambiFullReferenceQualityRunner` to run the full-reference version of CAMBI.

```text
dis_path = VmafConfig.test_resource_path("yuv", "KristenAndSara_1280x720_8bit_processed.yuv")
asset = Asset(dataset="test", content_id=0, asset_id=0,
              workdir_root=VmafConfig.workdir_path(),
              ref_path=dis_path,
              dis_path=dis_path,
              asset_dict={'width': 1280, 'height': 720,
                          'dis_enc_width': 960, 'dis_enc_height': 540})

self.qrunner = CambiQualityRunner(
    [asset, asset_original],
    None, fifo_mode=False,
    result_store=None,
    optional_dict={}
)
self.qrunner.run(parallelize=True)
results = self.qrunner.results

# score: arithmetic mean score over all frames
self.assertAlmostEqual(results[0]['Cambi_score'],
                        1.218365, places=4)
```

## CPU SIMD paths

The CPU extractor picks its kernels once, in `init()`, from the host's CPU
flags. Nothing needs to be configured: the scalar C code is the default and
the reference, and a SIMD kernel replaces it only on a CPU that supports its
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

The c-values stage is the expensive one, and all three SIMD drivers (AVX2,
AVX-512, NEON) are fast because they skip work, not because their vectors are
wider. On flat, banding-prone content almost every pixel leaves the sliding
histogram unchanged: it is masked out, outside the luma band CAMBI scores, or
equal to the pixel it replaces. The scalar driver visits every pixel; the SIMD
drivers test a block of up to 256 pixels with vector compares and update the
histogram only for the ones that change it. The histogram each row sees is the
same, so the scores are too. (Upstream's AVX2 driver, which visits every pixel,
is still built and tested but no longer used: in icx builds it was slower than
the scalar code.)

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

A whole CAMBI frame on that host, single thread, frames per second. "Before"
is the build without these kernels, at its default dispatch; "default" is
AVX-512 on this host; "AVX2 only" is `--cpumask 48`, what a CPU without
AVX-512 runs:

| Input | GCC, before | GCC, default | GCC, AVX2 only | Clang, AVX2 only | icx, AVX2 only (before) |
| --- | --- | --- | --- | --- | --- |
| 576x324 8-bit | 2024 | 2668 | 2409 | 2540 | 2437 (1388) |
| 1920x1088 8-bit | 191 | 252 | 229 | 240 | 227 (128) |
| 1920x1080 10-bit | 267 | 339 | 313 | 338 | 318 (180) |
| 3840x2176 8-bit | 46.8 | 60.5 | 56.4 | 57.4 | 56.7 (33.1) |

On aarch64 there was no hardware to time; under `qemu-aarch64` the NEON
kernels execute 9–24 % of the scalar instructions for the anti-dithering
filter, derivative row and decimate, and 21–44 % for the c-values stage.

The spatial-mask row kernels, measured on their own (1920x1080 frame, 7x7 mask
filter, [Research-2062](../research/2062-cambi-spatial-mask-simd.md)):

| Kernel | Scalar | AVX2 | AVX-512 |
| --- | --- | --- | --- |
| dp row, GCC build | 486 µs | 216 µs (2.25x) | 153 µs (3.18x) |
| dp row, Clang build | 428 µs | 236 µs (1.81x) | 165 µs (2.59x) |
| mask row, GCC build | 275 µs | 164 µs (1.68x) | 122 µs (2.26x) |
| mask row, Clang build | 251 µs | 167 µs (1.51x) | 121 µs (2.08x) |

To compare paths on your own machine, mask CPU flags with `--cpumask`, which
takes the flags to *disable*. The score must not change:

```bash
# default dispatch (AVX-512 or NEON where available)
vmaf -r ref.yuv -d dis.yuv -w 1920 -h 1080 -p 420 -b 8 \
    --no_prediction --feature cambi --precision max --json -o simd.json
# x86: AVX2 only (disable the AVX-512 flag, bit 4)
vmaf ... --cpumask 16 -o avx2.json
# x86: scalar only
vmaf ... --cpumask 65535 -o scalar.json
# aarch64: scalar only (disable NEON, bit 0)
vmaf ... --cpumask 1 -o scalar.json
```

The JSON files differ only in `fps`. The tests behind this are in the `simd`
suite (`python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- -C build --suite simd`): `test_cambi_stage_simd` compares
every per-stage kernel with the scalar stage, `test_cambi_spatial_mask_simd`
the spatial-mask rows, `test_cambi_simd` the c-values row, and
`test_cambi_dispatch_invariance` runs the whole extractor at each dispatch
level and requires identical scores.

## GPU support

CAMBI has a CUDA backend (T3-15a / [ADR-0360](../adr/0360-cambi-cuda.md)).

> **HIP scores before this release were wrong.** Up to and including v3.2.1 the
> HIP CAMBI twin returned **exactly `0.0`** on banding content that the CPU
> scores at `5.85`. It hand-rolled the TVI-threshold bisection over an inverted
> predicate seeded from luma 0 instead of `luma_range.foot`, producing
> `tvi_for_diff = [1026, 1025, 1024, 4]` where the CPU produces
> `[182, 309, 436, 563]`; that collapses the scored luma band from 564 entries
> to a handful, and `calculate_c_values()` then discards almost every pixel as
> out-of-band. Two smaller divergences compounded it: `filter_mode` filtered
> output rows 0 and `height-1`, which `cambi.c` leaves unfiltered, and the 7x7
> mask box sum clamped out-of-frame taps to the border pixel where the CPU's
> summed-area table zero-pads them. All three are fixed per
> [ADR-1219](../adr/1219-gpu-cambi-tvi-shared-bisection.md) and HIP is now
> bit-exact with the CPU. **Re-measure any CAMBI score taken on the HIP
> backend.** The Metal twin carried the first two and gets the same fixes;
> CUDA and SYCL were unaffected.

It uses the Strategy II hybrid architecture: the integer phases (spatial mask,
2× decimate, 3-tap separable mode filter) run on the GPU; the
precision-sensitive sliding-histogram `calculate_c_values` + top-K spatial
pool stay on the host. Cross-backend gate runs at `places=4`.

> **Note**: The Vulkan backend (formerly T7-36 / ADR-0210) was removed in
> [ADR-0726](../adr/0726-drop-vulkan-backend.md). CAMBI no longer has a Vulkan
> path. Use the CUDA backend for GPU-accelerated CAMBI scoring.

### CUDA

```bash
# Build with CUDA enabled
meson setup build-cuda core -Denable_cuda=true
ninja -C build-cuda

# Run with --backend cuda
./build-cuda/tools/vmaf -r ref.yuv -d dis.yuv -w W -h H \
    -p 420 -b 8 --backend cuda --feature cambi_cuda
```

**Implementation note (lusoris/vmaf#870):** `submit_fex_cuda` downloads the distorted
picture from device memory to a transient host copy before passing it to
`vmaf_cambi_preprocessing`. This is required because the host-side preprocessing
path (`decimate_generic_uint8_and_convert_to_10b`) dereferences `pic->data[0]`
as a host pointer; on a CUDA picture that field holds a `CUdeviceptr` (device
address), causing SIGSEGV. The download uses `vmaf_cuda_picture_download_async`
on the picture's private CUDA stream followed by `cuStreamSynchronize`. The host
copy is unreferenced before `submit_fex_cuda` returns. Cross-backend parity
versus CPU `cambi` is verified at `places=4` per ADR-0214.

Companion research digest:
[Research-0091](../research/0091-cambi-cuda-integration.md) (CUDA).
