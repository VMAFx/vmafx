<!-- markdownlint-disable MD013 MD060 -->
# Using VMAF with FFmpeg

After [installing](../../core/README.md#install) `libvmaf`, you can use it with [FFmpeg](http://ffmpeg.org/). Under the FFmpeg directory, configure, build and install FFmpeg with:

```bash
./configure --enable-libvmaf
make -j4
make install
```

Using FFmpeg+libvmaf is very powerful, as you can create complex filters to calculate VMAF directly on videos of different encoding formats and resolutions. For the best practices of computing VMAF at the right resolution, refer to our [tech blog](https://medium.com/netflix-techblog/vmaf-the-journey-continues-44b51ee9ed12).

We provide a few examples how you can construct the FFmpeg command line and use VMAF as a filter. Note that you may need to download the test videos from [vmaf_resource](https://github.com/Netflix/vmaf_resource/tree/master/python/test/resource).

Below is an example on how you can run FFmpeg+libvmaf on a pair of YUV files. First, download the reference video [`src01_hrc00_576x324.yuv`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/yuv/src01_hrc00_576x324.yuv) and the distorted video [`src01_hrc01_576x324.yuv`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/yuv/src01_hrc01_576x324.yuv). `-r 24` sets the frame rate (note that it needs to be before `-i`), and `PTS-STARTPTS` synchronizes the PTS (presentation timestamp) of the two videos (this is crucial if one of your videos does not start at PTS 0, for example, if you cut your video out of a long video stream). It is important to set the frame rate and the PTS right, since FFmpeg filters synchronize based on timestamps instead of frames.

The `log_path` is set to standard output `/dev/stdout`. The model is specified
using the preferred `model=version=` form (the older `model_path=` option is
legacy and still accepted but not recommended for new scripts).

```bash
ffmpeg -video_size 576x324 -r 24 -pixel_format yuv420p -i src01_hrc00_576x324.yuv \
    -video_size 576x324 -r 24 -pixel_format yuv420p -i src01_hrc01_576x324.yuv \
    -lavfi "[0:v]setpts=PTS-STARTPTS[reference]; \
            [1:v]setpts=PTS-STARTPTS[distorted]; \
            [distorted][reference]libvmaf=log_fmt=xml:log_path=/dev/stdout:model=version=vmaf_v0.6.1:n_threads=4" \
    -f null -
```

The expected output is:

```bash
[libvmaf @ 0x7fcfa3403980] VMAF score: 76.668905
```

Below is a more complicated example where the inputs are packaged `.mp4` files. It takes in 1) a reference video [`Seeking_30_480_1050.mp4`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/mp4/Seeking_30_480_1050.mp4) of 480p and 2) a distorted video [`Seeking_10_288_375.mp4`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/mp4/Seeking_10_288_375.mp4) of 288p upsampled to `720x480` using bicubic, and compute VMAF on the two 480p videos. Bicubic is used as the recommended upsampling method (also see the [techblog](https://medium.com/netflix-techblog/vmaf-the-journey-continues-44b51ee9ed12) for more details).

```bash
ffmpeg \
    -r 24 -i Seeking_30_480_1050.mp4 \
    -r 24 -i Seeking_10_288_375.mp4 \
    -lavfi "[0:v]setpts=PTS-STARTPTS[reference]; \
            [1:v]scale=720:480:flags=bicubic,setpts=PTS-STARTPTS[distorted]; \
            [distorted][reference]libvmaf=log_fmt=xml:log_path=/dev/stdout:model=version=vmaf_v0.6.1:n_threads=4" \
    -f null -
```

The expected output is:

```bash
[libvmaf @ 0x7fb5b672bc00] VMAF score: 51.017497
```

See the [FFmpeg's guide to libvmaf](https://ffmpeg.org/ffmpeg-filters.html#libvmaf), the [FFmpeg Filtering Guide](https://trac.ffmpeg.org/wiki/FilteringGuide) for more examples of complex filters, and the [Scaling Guide](https://trac.ffmpeg.org/wiki/Scaling) for information about scaling and using different scaling algorithms.

## `libvmaf` filter option reference

The `libvmaf` filter ships with FFmpeg (source:
[`libavfilter/vf_libvmaf.c`](https://github.com/FFmpeg/FFmpeg/blob/master/libavfilter/vf_libvmaf.c))
and wraps this repo's `libvmaf` C API. Options are set after the
filter name inside an `-lavfi` expression, colon-separated:

```text
libvmaf=model=version=vmaf_v0.6.1:log_path=/dev/stdout:log_fmt=json:n_threads=4
```

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `model` | string (pipe-separated `version=` / `path=`) | `version=vmaf_v0.6.1` | Load a built-in or file-backed model; supports stacked models. |
| `log_path` | path | (stderr only) | Where to write the per-frame report (`/dev/stdout` is common). |
| `log_fmt` | `xml` / `json` / `csv` / `sub` | `xml` | Report format; matches the `vmaf` CLI output modes. |
| `feature` | string (pipe-separated `name=` entries; escaped-colon intra-feature options) | (only model features) | Attach extra feature extractors. See [Feature option syntax](#feature-option-syntax) below. |
| `pool` | `mean` / `min` / `harmonic_mean` | `mean` | Pooling method for the per-frame scores. |
| `n_threads` | integer | `0` (library default) | Number of worker threads libvmaf is allowed to spawn. |
| `n_subsample` | integer `≥ 1` | `1` | Compute VMAF on every Nth frame only — useful for long-clip QC. |

The filter publishes the final pooled score to FFmpeg's log as
`VMAF score: <mean>`; the structured log at `log_path` is authoritative.

### Fork-added options

The fork's `ffmpeg-patches/` series (0001–0017) adds options to
the `libvmaf` filter beyond the upstream surface — tiny-AI ONNX
inference, backend selectors for SYCL / CUDA / HIP, a dedicated
`libvmaf_sycl` filter, and the Pelorus perceptual-weighting reader. (The `libvmaf_vulkan` filter and `vulkan_device`
option were removed in ADR-0726.) The options
below are from patches 0001–0003:

| Option | Default | Notes |
|---|---|---|
| `tiny_model=path` | none | ONNX path for the tiny-AI loader (`ffmpeg-patches/0001-...`). |
| `tiny_device=auto\|cpu\|cuda\|openvino\|rocm` | `auto` | ORT device selector for the tiny model. |
| `tiny_threads=N` | `0` | CPU-EP intra-op thread count (`0` = ORT default). |
| `tiny_fp16=0\|1` | `0` | Request fp16 I/O when the device supports it. |
| `sycl_device=N` | `-1` | SYCL device index, `-1` = system default (`ffmpeg-patches/0003-...`). |
| `sycl_profile=0\|1` | `0` | Enable SYCL queue profiling (`ffmpeg-patches/0003-...`). |
| ~~`vulkan_device=N`~~ | removed | Removed in ADR-0726. The `ffmpeg-patches/0004-...` patch is now a no-op shim per ADR-0860. |
| `cuda=0\|1` | `0` | Enable the CUDA backend on the libvmaf filter (`ffmpeg-patches/0010-...`). |
| `hip_device=N` | `-1` | HIP device index, `-1` = system default (`ffmpeg-patches/0011-...`). |
| `metal_device=N` | `-2` | Metal device index, `-2` = disabled, `-1` = system default, `≥0` = explicit (`ffmpeg-patches/0012-...`). The `-2` default is a fork-local convention because Metal is auto-disabled on Linux; an unset value should not enable the backend. |
| `perceptual_weight=0\|1` | `0` | Perceptually re-weight VMAF **spatial pooling** using Pelorus side-data (per-cell banding-risk / variance maps) carried on the distorted frame as an unregistered SEI (`ffmpeg-patches/0017-...`, [ADR-1118](../adr/1118-perceptual-sidedata-weighting.md)). OFF by default and **golden-isolated**: weighting engages only when this is `1` **and** a valid Pelorus blob is present, so standard scoring (and the Netflix golden pairs, which carry no side-data) is byte-identical to upstream. A foreign or absent SEI is ignored and the frame is scored unweighted. Pair with a Pelorus-enabled producer that emits the interop blob; see [`docs/api/perceptual-weight.md`](../api/perceptual-weight.md) for the C-API. |

Backend selectors live alongside in a small dedicated table —
see "Backend selectors on the libvmaf filter" below.

The fork-specific `--precision` flag on the `vmaf` CLI does not
have an FFmpeg filter equivalent; pooled-score precision from
FFmpeg comes from FFmpeg's own `%f` formatting. Use the `vmaf`
CLI (not the filter) when you need `--precision=max` round-trip
lossless output (CLI default is `%.6f` for Netflix-compat per
[ADR-0119](../adr/0119-cli-precision-default-revert.md); pass
`--precision=max` to opt into `%.17g`).

### Feature option syntax

The `feature=` option attaches extra extractors beyond the model's intrinsic
features. Values are pipe-separated `name=<extractor>` entries (e.g.
`feature='name=psnr|name=ciede|name=adm3'`). Intra-feature options use
backslash-escaped colons inside the value, e.g.
`feature='name=integer_ssim\:enable_chroma=true'`. Names match the entries in
libvmaf's `feature_extractor_list[]`.

### Dedicated SYCL filter: libvmaf_sycl

The fork ships a dedicated `libvmaf_sycl` filter (FFmpeg patch in
`ffmpeg-patches/`) that routes scoring through libvmaf's SYCL backend without
invoking the generic `libvmaf` filter's CPU path. Usage mirrors `libvmaf=`
syntax but accepts an additional `sycl_device=<n>` option. See
[`vmaf-vpl.md`](vmaf-vpl.md) for the SYCL backend overview.

### Multi-feature / multi-model examples

Score a pair with the default model plus PSNR + CIEDE attached:

```bash
ffmpeg -i ref.y4m -i dis.y4m \
  -lavfi "[0:v][1:v]libvmaf=feature='name=psnr|name=ciede':log_fmt=json:log_path=/dev/stdout" \
  -f null -
```

Score against two models in one pass (both appear in the report):

```bash
ffmpeg -i ref.y4m -i dis.y4m \
  -lavfi "[0:v][1:v]libvmaf=model='version=vmaf_v0.6.1|version=vmaf_v0.6.1neg':log_fmt=json:log_path=/dev/stdout" \
  -f null -
```

### When to use `vmaf` CLI instead of the filter

- **You need `--precision=max`** (round-trip lossless `%.17g` output;
  CLI default is `%.6f` per
  [ADR-0119](../adr/0119-cli-precision-default-revert.md)).
- **You need the tiny-AI surface** (`--tiny-model`, `--tiny-device`,
  `--no-reference`) — the FFmpeg filter does not yet expose these
  flags. See [api/dnn.md](../api/dnn.md) for the C-API surface.
- **You want deterministic CPU-only output.** The `libvmaf_cuda` filter
  is automatic inside FFmpeg based on frame format; the CLI gives you
  explicit `--no_cuda` / `--no_sycl` opt-out control.

For everything else, the filter is the right tool.

## Note about the model path on Windows

Due to Windows not having a good default for where to pull the VMAF model from, you will always need to specify `model_path` when calling libvmaf through `ffmpeg`. However, you will need to be careful about the path you pass to `model_path`.

If you are using a relative path for your `model_path`, you can completely ignore this whole section, else if you are trying to use an absolute Windows path (`D:\mypath\vmaf_v0.6.1.json`) for your `model_path` argument, you will need to be careful so `ffmpeg` passes the right path to `libvmaf`.

The final command line will depend on what shell you are running `ffmpeg` through, so you will need to go through the following steps to make sure your path is okay.

1. Convert all of the backslashes `\` to forward slashes `/` (`D:/mypath/vmaf_v0.6.1.json`)
2. Escape the colon `:` character by using a backslash `\` (`D\:/mypath/vmaf_v0.6.1.json`)
3. Then escape that backslash with another backslash (`D\\:/mypath/vmaf_v0.6.1.json`)
4. The next step will depend on the shell that will run `ffmpeg`:
    - For PowerShell and Command Prompt, this will be enough and your final `ffmpeg` command line will look something like

    ```powershell
    ./ffmpeg.exe -i dist.y4m -i ref.y4m \
        -lavfi libvmaf=model_path="D\\:/mypath/vmaf_v0.6.1.json" \
        -f null -
    ```

    Note: I only quoted the path part for trivial reasons and in this specific case, it can be unquoted or you can quote the whole part after lavfi starting from `libvmaf` to `json` and it should give the same result due to neither shell treating the `\` as a special character

    - For bash or specifically msys2 bash, it has some additional considerations. The first thing to know is that bash treats the backslash character `\` a bit special in that it's an escape character normally when not put inside single quotes. The second thing to know is that msys2's bash attempts convert a posix-like path (`/mingw64/share/model/vmaf_v0.6.1.json`) to a Windows mixed path (`D:/msys2/mingw64/share/model/vmaf_v0.6.1.json`) when passing arguments to a program. Normally, this would be fine, however, in our case, this works against us since we cannot allow it to convert the path to a normal path with an un-escaped colon. For this, we will need to not only escape the escaped backslash, but we will also need to pass the `MSYS2_ARG_CONV_EXCL` environment variable with the value of `*` to make sure it doesn't apply that special conversion on any of the arguments

    ```bash
    MSYS2_ARG_CONV_EXCL="*" \
        ./ffmpeg.exe -i dist.y4m -i ref.y4m -lavfi \
        libvmaf=model_path="D\\\:/mypath/vmaf_v0.6.1.json" -f null -
    ```

    Note: in this case, the quotes are not as trivial as the PowerShell/cmd version, as removing the quotes entirely will require you to re-escape the backslash resulting in 4 total backslashes, but quoting the whole argument will be fine.

    Second Note: if you use single quotes around the path, it will be fine as well and the final command line would look like

    ```bash
    MSYS2_ARG_CONV_EXCL="*" \
        ./ffmpeg.exe -i dist.y4m -i ref.y4m -lavfi \
        libvmaf=model_path='D\\:/mypath/vmaf_v0.6.1.json' -f null -
    ```

    with only a double backslash instead of a triple.

## GPU-accelerated VMAF through FFmpeg (fork-specific)

### Per-backend copy-paste examples

One ready-to-paste invocation per backend. All examples use the same
input pair (`reference.mp4`, `distorted.mp4`); each routes the work to a
different compute path. The fork-added `sycl_device=`, `cuda=`, and
`hip_device=` options come from patches `0003`, `0010`, and `0011`.
(The `vulkan_device=` option from patch `0004` was removed in ADR-0726;
patch `0004` is retained as a no-op shim per ADR-0860.)

**CPU (default — no hwaccel, no GPU build needed):**

```bash
ffmpeg -i reference.mp4 -i distorted.mp4 \
       -filter_complex "[0:v][1:v]libvmaf=log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

**CUDA (NVIDIA only — built with `-Denable_cuda=true`; uses the
dedicated `libvmaf_cuda` filter to keep frames device-resident):**

```bash
ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i reference.mp4 \
       -hwaccel cuda -hwaccel_output_format cuda -i distorted.mp4 \
       -filter_complex "[0:v][1:v]libvmaf_cuda=log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

**CUDA — software input (built with `-Denable_cuda=true` and FFmpeg
configured `--enable-libvmaf-cuda`; uses the fork-added `cuda=1`
selector on the regular `libvmaf` filter — runs CUDA feature kernels
without the CUDA hwaccel decode round-trip):**

```bash
ffmpeg -i reference.mp4 -i distorted.mp4 \
       -filter_complex "[0:v][1:v]libvmaf=cuda=1:log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

When `cuda=1` the filter inits a `VmafCudaState` against the CUDA
primary context on the default device, imports it into the
`VmafContext`, and dispenses `VmafPicture`s from a `HOST_PINNED`
preallocation pool so the existing copy loop fills pinned-host memory
the CUDA feature kernels DMA from without a staging copy. Mirrors
the `sycl_device=N` selector; device selection is via
`CUDA_VISIBLE_DEVICES` at process scope (the upstream
`VmafCudaConfiguration` C-API has no `device_index` field).

**SYCL (Intel / Arc — built with `-Denable_sycl=true`; uses the
fork-added `sycl_device=N` selector on the regular `libvmaf` filter,
fed by software-decoded frames):**

```bash
ffmpeg -i reference.mp4 -i distorted.mp4 \
       -filter_complex "[0:v][1:v]libvmaf=sycl_device=0:log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

**Vulkan — removed (ADR-0726):** The `vulkan_device=N` filter option and
the `-Denable_vulkan=enabled` build flag no longer exist. Historical
examples are preserved in git history. The `libvmaf` filter patch
(`ffmpeg-patches/0004-...`) is now a no-op shim per ADR-0860; it contributes
no linked code but is kept so downstream patches' context lines stay intact.

To list the full set of options the locally-installed `libvmaf` filter
exposes (useful when an option in this doc has drifted from the binary):

```bash
ffmpeg -h filter=libvmaf
```

### Hardware decode + GPU compute (faster pipeline)

The four examples above use software decode (FFmpeg's CPU codecs).
For long clips or 4K+ inputs it's usually faster to use FFmpeg's
hardware decoders, then bridge to the libvmaf compute backend.

**CUDA — zero-copy end-to-end (decode + compute on the same GPU):**

```bash
ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i reference.mp4 \
       -hwaccel cuda -hwaccel_output_format cuda -i distorted.mp4 \
       -filter_complex "[0:v][1:v]libvmaf_cuda=log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

**QSV / VAAPI decode (Intel) → zero-copy SYCL compute via the
dedicated `libvmaf_sycl` filter (no CPU readback —
`ffmpeg-patches/0005-libvmaf-add-libvmaf-sycl-filter.patch`):**

```bash
ffmpeg -hwaccel qsv -hwaccel_output_format qsv -i reference.mp4 \
       -hwaccel qsv -hwaccel_output_format qsv -i distorted.mp4 \
       -filter_complex "[0:v][1:v]libvmaf_sycl=log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

The `libvmaf_sycl` filter consumes oneVPL `mfxFrameSurface1`
frames directly (`AVFrame->data[3]`), extracts the underlying
VA surface ID, and routes through `vmaf_sycl_import_va_surface`
for zero-copy DMA-BUF import on the Level Zero / SYCL compute
queue. Build FFmpeg with `--enable-libvmaf-sycl` (in addition
to `--enable-libvmaf`) to enable it. Plain VAAPI decode without
the QSV wrapping still requires the `hwdownload,format=yuv420p`
bridge below — the filter currently consumes oneVPL surfaces
specifically.

**Plain VAAPI decode (AMD or non-QSV Intel paths) → SYCL compute
(software-frame bridge):**

```bash
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i reference.mp4 \
       -hwaccel vaapi -hwaccel_output_format vaapi -i distorted.mp4 \
       -filter_complex "[0:v]hwdownload,format=yuv420p[r]; \
                        [1:v]hwdownload,format=yuv420p[d]; \
                        [r][d]libvmaf=sycl_device=0:log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

Active GPU backends that ship a dedicated filter consuming hwdec frames
directly without the `hwdownload,format=yuv420p` round-trip:

- `libvmaf_cuda` — CUDA frames.
- `libvmaf_sycl` — QSV / oneVPL frames (T7-28,
  `ffmpeg-patches/0005-libvmaf-add-libvmaf-sycl-filter.patch`).
- `libvmaf_metal` — `AV_PIX_FMT_VIDEOTOOLBOX` frames (T8-IOS,
  `ffmpeg-patches/0013-libvmaf-add-libvmaf-metal-filter.patch`).
  Routes through the `vmaf_metal_picture_import` C API; on hosts
  without an Apple-Family-7 MTLDevice the filter fails fast at
  `config_props` with `AVERROR(ENODEV)`. See
  [ADR-0423](../adr/0423-metal-iosurface-import-scaffold.md).

> **`libvmaf_vulkan` removed (ADR-0726):** The `libvmaf_vulkan` filter
> (`ffmpeg-patches/0006-...`) and `--enable-libvmaf-vulkan` configure flag
> are gone. Patch `0006` is retained as a no-op shim per ADR-0860.

**With `libvmaf_metal` (VideoToolbox hwdec):**

```bash
ffmpeg -hwaccel videotoolbox -hwaccel_output_format videotoolbox -i reference.mp4 \
       -hwaccel videotoolbox -hwaccel_output_format videotoolbox -i distorted.mp4 \
       -filter_complex "[0:v][1:v]libvmaf_metal=log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

Build FFmpeg with `--enable-libvmaf-metal` against a libvmaf compiled
with `-Denable_metal=enabled`. The filter pulls the `IOSurfaceRef`
backing each `CVPixelBufferRef` via `CVPixelBufferGetIOSurface` and
routes it through `vmaf_metal_picture_import` — which locks the
surface read-only and memcpys each plane into a shared-storage
`VmafPicture` (the unified-memory cost on Apple Silicon is
equivalent to a Shared MTLBuffer copy). The libvmaf-side runtime
falls back to `MTLCreateSystemDefaultDevice` until upstream FFmpeg
ships an `AVMetalDeviceContext`; on multi-GPU Mac Pro hosts this
may pick a different MTLDevice than the VideoToolbox decoder used
— the same-device contract the Vulkan filter solves via
`AVVulkanDeviceContext` is documented as a follow-up here. On
non-Apple-Family-7 hosts the filter exits at `config_props` time
with `AVERROR(ENODEV)` and a pointer to
[ADR-0423](../adr/0423-metal-iosurface-import-scaffold.md).

### Background

When `libvmaf` is built with `-Denable_cuda=true`, FFmpeg exposes the
`libvmaf_cuda` filter, which keeps frames on the GPU end-to-end when the
decoder is also CUDA-backed (e.g. `-hwaccel cuda`). The CPU / SYCL / HIP
examples in the first section all use the regular `libvmaf` filter; only
the selector option (`sycl_device=N` / `hip_device=N`) changes which compute
path libvmaf takes internally. Decoded frames are software-decoded in those
cases and copied into device memory by libvmaf itself.

For SYCL specifically: `sycl_device=-1` keeps the CPU path. Setting
`sycl_device=N` (any non-negative ordinal) opts in. See
[backends/sycl/overview.md](../backends/sycl/overview.md) for device
enumeration.

### Selector option reference

The same fork-added selector pattern exists for SYCL, CUDA, and HIP on the
`libvmaf` filter itself, contributed by
`ffmpeg-patches/0003-libvmaf-wire-sycl-backend-selector.patch`,
`0010-libvmaf-wire-cuda-backend-selector.patch`, and
`0011-libvmaf-wire-hip-backend-selector.patch`. The Vulkan selector
(`0004`) was removed in ADR-0726 (retained as a no-op shim per ADR-0860):

| Option | Default | Notes |
|---|---|---|
| `sycl_device=N` | `-1` (disabled) | Pick SYCL device ordinal; `-1` keeps the CPU path. Errors out if libvmaf was built without `-Denable_sycl=true`. Patch `0003`. |
| `sycl_profile=0\|1` | `0` | Enable SYCL queue profiling. Patch `0003`. |
| `cuda=0\|1` | `0` | Enable CUDA compute path on software-decoded input. Patch `0010`. |
| `hip_device=N` | `-1` (disabled) | Pick HIP device ordinal; `-1` keeps the CPU path. Errors out if libvmaf was built without `-Denable_hip=true`. Patch `0011` ([ADR-0380](../adr/0380-ffmpeg-hip-backend-selector.md)). |

## External resources

Refer to [this page](external-resources.md) for a list of FFmpeg-based 3rd-party tools.
