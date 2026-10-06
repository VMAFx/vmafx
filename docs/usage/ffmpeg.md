<!-- markdownlint-disable MD013 MD046 MD060 -->
# Using VMAF with FFmpeg

Compute VMAF inside an FFmpeg filter graph with the `libvmaf` filter, or with
one of the fork's GPU filters. This page covers the input-pad order, the first
commands to run, every filter option, the per-backend filters and the Windows
model-path escaping.

[FFmpeg](http://ffmpeg.org/) filters can score videos of different encodings
and resolutions directly. For the best practices of computing VMAF at the right
resolution, see
the Netflix [tech
blog](https://medium.com/netflix-techblog/vmaf-the-journey-continues-44b51ee9ed12).
Test videos are in
[vmaf_resource](https://github.com/Netflix/vmaf_resource/tree/master/python/test/resource).

## Input ordering (read this first) {#input-ordering}

!!! warning "FFmpeg takes the distorted video first and the reference second"
    The two input pads on `libvmaf`, `libvmaf_cuda`, `libvmaf_sycl`,
    `libvmaf_metal` and `libvmaf_tune` are the opposite of the Python runner
    and the standalone `vmaf` CLI. FFmpeg pad 0 is the **distorted/main**
    stream and pad 1 is the **reference** stream. The Python and CLI surfaces
    take the reference first.

    Reversing the pads does not fail. It changes the direction of the
    comparison and can silently inflate the result: the measured Netflix-pair
    example in
    [Research-0730](../research/0730-ffmpeg-libvmaf-smoke-20260527.md#bug-2-input-ordering-trap-libvmaf-filter-reverses-refdis-vs-python-runner)
    produced `83.782079` in the wrong direction instead of the correct
    `76.667830`.

Correct: the distorted input feeds pad 0 and the reference input feeds pad 1.

```bash
ffmpeg -i distorted.mp4 -i reference.mp4 \
  -filter_complex "[0:v][1:v]libvmaf=log_fmt=json:log_path=/dev/stdout" \
  -f null -
```

Incorrect: shown only so the dangerous inversion is recognizable. Do not use
this ordering.

<!-- vmafx-ffmpeg-input-order: intentionally-wrong -->
```bash
ffmpeg -i reference.mp4 -i distorted.mp4 \
  -filter_complex "[0:v][1:v]libvmaf=log_fmt=json:log_path=/dev/stdout" \
  -f null -
```

The maintained FFmpeg patch also logs the pad contract at filter
initialization, but that message cannot repair an already reversed command.

## Install

Stock FFmpeg builds the upstream `libvmaf` filter once
[`libvmaf` is installed](../../core/README.md#install):

```bash
./configure --enable-libvmaf
make -j4
make install
```

The fork-added options, filters and GPU selectors on this page need FFmpeg
patched with the fork's series, `ffmpeg-patches/0001` to `0020` listed in
`ffmpeg-patches/series.txt`, applied in order to a clean `n9.0.2` checkout:

```bash
cd /path/to/ffmpeg && git checkout n9.0.2
while IFS= read -r patch; do
    case "$patch" in ""|\#*) continue ;; esac
    git am --3way "/path/to/vmaf/ffmpeg-patches/$patch" || exit 1
done < /path/to/vmaf/ffmpeg-patches/series.txt
./configure --enable-libvmaf
make -j4
```

See [FFmpeg patch automation](../development/ffmpeg-patch-automation.md) for
how the series tracks the stable release. Patches `0004` and `0006` are
no-op shims since the Vulkan backend was removed (see
[History](#history)).

To list the options of the locally installed filter, which is useful when this
page has drifted from the binary:

```bash
ffmpeg -h filter=libvmaf
```

## First commands

### A pair of YUV files

Download the reference
[`src01_hrc00_576x324.yuv`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/yuv/src01_hrc00_576x324.yuv)
and the distorted
[`src01_hrc01_576x324.yuv`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/yuv/src01_hrc01_576x324.yuv)
video, then run:

```bash
ffmpeg -video_size 576x324 -r 24 -pixel_format yuv420p -i src01_hrc00_576x324.yuv \
    -video_size 576x324 -r 24 -pixel_format yuv420p -i src01_hrc01_576x324.yuv \
    -lavfi "[0:v]setpts=PTS-STARTPTS[reference]; \
            [1:v]setpts=PTS-STARTPTS[distorted]; \
            [distorted][reference]libvmaf=log_fmt=xml:log_path=/dev/stdout:model=version=vmaf_v0.6.1:n_threads=4" \
    -f null -
```

- `-r 24` sets the frame rate and must come before `-i`.
- `setpts=PTS-STARTPTS` synchronizes the presentation timestamps of the two
  videos. This is crucial if a video does not start at PTS 0, for example when
  it was cut from a long stream. FFmpeg filters synchronize on timestamps, not
  on frame counts.
- `log_path` is standard output (`/dev/stdout`).
- The model uses the preferred `model=version=` form. The older `model_path=`
  option is legacy and still accepted, but not recommended for new scripts.

The expected output (`vmaf_v0.6.1` on the Netflix pair, matching the `vmaf`
CLI's `76.66783025`):

```text
[libvmaf @ 0x7fcfa3403980] VMAF score: 76.667830
```

### Packaged `.mp4` inputs

This example takes a 480p reference
[`Seeking_30_480_1050.mp4`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/mp4/Seeking_30_480_1050.mp4)
and a 288p distorted video
[`Seeking_10_288_375.mp4`](https://github.com/Netflix/vmaf_resource/blob/master/python/test/resource/mp4/Seeking_10_288_375.mp4),
upsamples the distorted one to `720x480` with bicubic, and computes VMAF on the
two 480p videos. Bicubic is the recommended upsampling method; see the
[tech blog](https://medium.com/netflix-techblog/vmaf-the-journey-continues-44b51ee9ed12).

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

```text
[libvmaf @ 0x7fb5b672bc00] VMAF score: 51.017497
```

See [FFmpeg's guide to libvmaf](https://ffmpeg.org/ffmpeg-filters.html#libvmaf),
the [FFmpeg Filtering Guide](https://trac.ffmpeg.org/wiki/FilteringGuide) for
more complex filters, and the
[Scaling Guide](https://trac.ffmpeg.org/wiki/Scaling) for scaling algorithms.

## Filters at a glance

| Filter | Patch | Configure flag | Computes on |
| --- | --- | --- | --- |
| `libvmaf` | upstream + fork options | `--enable-libvmaf` | CPU by default; GPU through selector options |
| `libvmaf_cuda` | `0010` | `--enable-libvmaf-cuda` | CUDA, device-resident frames |
| `libvmaf_sycl` | `0005` | `--enable-libvmaf-sycl` | SYCL, QSV / oneVPL frames |
| `libvmaf_metal` | `0013` | `--enable-libvmaf-metal` | Metal, VideoToolbox frames |
| `libvmaf_tune` | `0008` | `--enable-libvmaf` | CPU; recommends a CRF for the next encode pass |
| `vmaf_pre` | `0002` | `--enable-libvmaf` (libvmaf 3.0.0+ with DNN) | Learned pre-filter (ONNX) |

## `vmafx` filter option table

The `vmafx` filter replaces the `vmaf`-named filters in the 1.0.0 series
(RC4, [ADR-1852](../adr/1852-vmafx-api-redesign.md)); its options are
generated from the option groups of `core/api/vmafx.toml` into
`ffmpeg-patches/src/vf_vmafx_options.h`, the table the filter compiles. The
filter itself lands in a later RC4 change; until then this table is the
contract it implements, and the `libvmaf` filter below is what the patched
FFmpeg builds today. Upstream option names (`n_threads`, `n_subsample`) stay
accepted as aliases.

<!-- BEGIN GENERATED: vmafx-api vmafx filter options (scripts/codegen/vmafx-api.py) -->

| Option | Aliases | Value | Default | Description |
| --- | --- | --- | --- | --- |
| `model` | | string | library default (`VMAF_DEFAULT_MODEL_VERSION`) | Model, colon-delimited: version= a built-in model, path= a model file, name= the name in the report, disable_clip, enable_transform, &lt;feature&gt;.&lt;option&gt;=&lt;value&gt; overloads. Several models score in one pass (repeat the option; the filter separates them with \|). |
| `backend` | | `auto` \| `cpu` \| `cuda` \| `sycl` \| `hip` \| `metal` | `auto` | Backend: auto uses the available ones; any other value runs that backend alone and fails when it is not available. |
| `feature` | | string | | Additional feature extractor, name[=key=value:...] (for example psnr or cambi=full_ref=true); several may be given (the filter separates them with \|). Mutually exclusive with the CTC presets. |
| `tiny_model` | | string | | Tiny ONNX model to load alongside the classic models. |
| `tiny_device` | | `auto` \| `cpu` \| `cuda` \| `openvino` \| `openvino-npu` \| `openvino-cpu` \| `openvino-gpu` \| `coreml` \| `coreml-ane` \| `coreml-gpu` \| `coreml-cpu` \| `rocm` | `auto` | ONNX Runtime execution provider of the tiny model. |
| `tiny_threads` | | uint | | Intra-op threads of the CPU execution provider (0: the runtime's default). |
| `tiny_fp16` | | bool | `false` | Request fp16 input and output where the execution provider supports it. |
| `threads` | `n_threads` | uint | | Worker threads of the feature extractors, capped to the hardware threads (0: score in the calling thread). |
| `cpumask` | | uint | | Bitmask of CPU instruction sets the extractors must not use. |
| `gpumask` | | uint | | Bitmask of GPU operations the extractors must not use. |
| `device` | | string | `auto` | GPU of the selected backend: auto, or a device index. Needs a GPU backend that selects devices by index (sycl, hip, metal). |
| `subsample` | `n_subsample` | uint >= 1 | `1` | Score every n-th frame (1: every frame). |
| `log_path` | | string | | Report file. |
| `log_fmt` | | `json` \| `xml` \| `csv` \| `sub` | `json` | Report format; json and xml carry the backend receipt. |
| `view_distance` | | float 0.75..24 | | Viewing distance in display heights (ADM adm_norm_view_dist). Unset: the model's value. |
| `display_height` | | uint >= 1 | | Height of the reference display in pixels (ADM adm_ref_display_height). Unset: the model's value. |
| `target_width` | | uint | `0` | Width of the target display the distorted video is scaled to (0: no scaling). Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `target_height` | | uint | `0` | Height of the target display the distorted video is scaled to (0: no scaling). Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `target_scaling` | | `none` \| `bilinear` \| `bicubic` \| `lanczos` | `none` | Scaling filter towards the target display. Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `pool` | | `min` \| `max` \| `mean` \| `harmonic_mean` \| `median` \| `perc5` \| `perc10` \| `perc20` | `mean` | Pool methods of the final score and of the windows. |
| `n_stats` | | float >= 0 | `0.0` | Window length in seconds (0: no windows). |
| `n_stats_frames` | | uint | `0` | Window length in frames (0: no windows); exclusive with n_stats. |
| `stats_out` | | `log` \| `metadata` \| `file` | `log` | Where window statistics go. |
| `stats_path` | | string | | NDJSON file of the window statistics, one object per window. |
| `score_fmt` | | string | `%.6f` | printf format of the scores (%.17g is lossless). |
| `provenance` | | `log` \| `report` | `log+report` | Print the provenance record at init (log) and embed it in the report (report). |
| `metadata` | | bool | `false` | Attach per-frame scores to the frames as lavfi.vmafx.&lt;metric&gt; metadata (delays output by the retention depth). |
| `profile` | | bool | `false` | Log the device timing breakdown at uninit. |
| `import` | | `auto` \| `device` \| `host` | `auto` | auto imports hardware frames without a copy and uploads software frames; device refuses software frames; host downloads hardware frames (logged once with the reason). |
| `perceptual_weight` | | bool | `false` | Weight the scores with the perceptual side data of the frames. |

<!-- END GENERATED: vmafx-api vmafx filter options -->

## `libvmaf` filter option reference

The `libvmaf` filter ships with FFmpeg (source:
[`libavfilter/vf_libvmaf.c`](https://github.com/FFmpeg/FFmpeg/blob/master/libavfilter/vf_libvmaf.c))
and wraps this repo's `libvmaf` C API. Options follow the filter name inside an
`-lavfi` expression, colon-separated:

```text
libvmaf=model=version=vmaf_v0.6.1:log_path=/dev/stdout:log_fmt=json:n_threads=4
```

### Common options

These options exist on every vmaf filter (`libvmaf`, `libvmaf_sycl`,
`libvmaf_metal`).

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `model` | string (pipe-separated `version=` / `path=`) | `version=vmaf_v0.6.1` | Load a built-in or file-backed model; supports stacked models. |
| `log_path` | path | (stderr only) | Where to write the per-frame report (`/dev/stdout` is common). |
| `log_fmt` | `xml` / `json` / `csv` / `sub` | `xml` | Report format; matches the `vmaf` CLI output modes. |
| `feature` | string (pipe-separated `name=` entries) | (only model features) | Attach extra feature extractors; see [Feature option syntax](#feature-option-syntax). |
| `pool` | `mean` / `min` / `max` / `harmonic_mean` / `median` / `perc5` / `perc10` / `perc20` | `mean` | Pooling method for the per-frame scores; see [Pooling](#pooling). |
| `n_threads` | integer | `0` (library default) | Number of worker threads libvmaf may spawn. |
| `n_subsample` | integer `>= 1` | `1` | Compute VMAF on every Nth frame only; useful for long-clip QC. |
| `score_fmt` | printf format string | unset (`%.6f`) | Format of the scores in the `log_path` report (patch `0016`); see [Score precision](#score-precision). |
| `cpumask` | integer bitmask | `0` (all enabled) | Disable SIMD ISAs: 1 = SSE2/NEON, 2 = SSE3, 4 = SSE4.1, 8 = AVX2, 16 = AVX-512, 32 = AVX-512ICL (patch `0014`). |
| `gpumask` | integer bitmask | `0` (all enabled) | Disable GPU dispatch: `1` disables CUDA (patch `0014`). |

The filter publishes the final pooled score to FFmpeg's log as
`VMAF score: <mean>`; the structured log at `log_path` is authoritative. After
an error it publishes neither; see
[When a frame cannot be scored](#when-a-frame-cannot-be-scored).

### Input colour tags and HDR models

A model file may declare a `conversion_target` (see
[model files](../models/v1.md#model-declared-conversion-target)). For such a
model the `libvmaf` filter (and the software path of `libvmaf_sycl`) reads the
colour properties of the first frame of each input (range, primaries, transfer
and matrix) and hands them to libvmaf, which converts both inputs to the
model's colorspace before scoring. Tag untagged files with `setparams` or
`-color_primaries`, `-color_trc`, `-colorspace` and `-color_range` on the
input. An input whose four properties are not all ones libvmaf knows (range
`tv` or `pc`; primaries `bt709`, `bt2020` or `smpte432`; transfer `bt709` or
`smpte2084`; matrix `bt709`, `bt2020nc` or `ictcp`) is left unspecified: a model
without a `conversion_target`, which is every shipped model, scores exactly as
before, and a model with one fails with an error naming the missing
attributes. The conversion needs a libvmaf built with `-Denable_zimg=true`, and
it runs on host frames: the `libvmaf_cuda` and `libvmaf_metal` filters and the
SYCL zero-copy path score device frames and refuse such a model.

### Pooling

The four order-statistic values (`median`, `perc5`, `perc10`, `perc20`) come
from [ADR-1188](../adr/1188-percentile-pooling-methods.md) and need
`ffmpeg-patches/0018-libvmaf-map-percentile-pool-methods.patch` in the applied
series. They interpolate linearly between ranks, so `perc10` is the same
"worst 10% of frames" number the Python harness reports.

Percentile pooling is available when the libvmaf headers define
`VMAF_HAVE_PERCENTILE_POOLING`. Apply the complete ordered series: patch 0018
adds the compatibility guard to the mappings introduced by patch 0005. Without
that macro, percentile strings keep the `mean` fallback. An unrecognised string
falls back to `mean`, as upstream does.

### Score precision

`score_fmt` (patch `0016`) sets the printf format used for the scores in the
`log_path` report. Unset, it is `%.6f`, the CLI default; pass
`score_fmt=%.17g` for the lossless output that `vmaf --precision=max` writes
(see [ADR-0119](../adr/0119-cli-precision-default-revert.md) and
[precision.md](precision.md)). The `VMAF score: <mean>` line in FFmpeg's own
log is formatted by the filter and is not affected.

### When a frame cannot be scored

If the filter cannot copy a frame or hand it to libvmaf, it stops. The log
shows one error naming the frame and the error, and FFmpeg exits non-zero:

```text
[Parsed_libvmaf_0] libvmaf: vmaf_read_pictures of frame 4 failed (Input/output error); the filter stops
[Parsed_libvmaf_0] libvmaf: no pooled score: the filter stopped on the error above
```

The filter then prints no `VMAF score:` line and writes no `log_path` report.
Upstream FFmpeg pools the frames read before the error and prints their score.
This fork does not (patch `0021`,
[ADR-1768](../adr/1768-ffmpeg-libvmaf-no-score-after-error.md)): that score
covers fewer frames than were decoded, and when the pooling itself fails
upstream prints a meaningless value.

Two related cases behave the same way:

- **The end-of-stream flush fails.** The filter logs
  `flushing libvmaf after frame <n> failed (<error>); no pooled score` and
  prints no score and no report. The flush runs while the filter graph is torn
  down, when the filter can no longer change FFmpeg's exit status, so this run
  can exit 0. Check for the score line or the report file, not only the exit
  status.
- **One model's pooled score fails.** That model gets no `VMAF score:` line,
  and no report is written.

`libvmaf_cuda` follows the same rules. `libvmaf_sycl` and `libvmaf_metal` stop
the same way
([ADR-1761](../adr/1761-sycl-filter-import-retry-then-fail.md)).

### Feature option syntax

`feature=` attaches extra extractors beyond the model's intrinsic features.
Values are pipe-separated `name=<extractor>` entries, for example
`feature='name=psnr|name=ciede|name=adm3'`. Intra-feature options use
backslash-escaped colons inside the value, for example
`feature='name=integer_ssim\:enable_chroma=true'`. Names match the entries in
libvmaf's `feature_extractor_list[]`.

### Multi-feature and multi-model examples

Score a pair with the default model plus PSNR and CIEDE attached:

```bash
ffmpeg -i dis.y4m -i ref.y4m \
  -lavfi "[0:v][1:v]libvmaf=feature='name=psnr|name=ciede':log_fmt=json:log_path=/dev/stdout" \
  -f null -
```

Score against two models in one pass (both appear in the report):

```bash
ffmpeg -i dis.y4m -i ref.y4m \
  -lavfi "[0:v][1:v]libvmaf=model='version=vmaf_v0.6.1|version=vmaf_v0.6.1neg':log_fmt=json:log_path=/dev/stdout" \
  -f null -
```

### Fork-added options

The fork's `ffmpeg-patches/` series (0001 to 0020) adds options to the `libvmaf`
filter beyond the upstream surface: tiny-AI ONNX inference, backend selectors
for SYCL / CUDA / HIP / Metal, per-ISA masks, a score format, and the Pelorus
perceptual-weighting reader. The table lists them with their patch.

| Option | Default | Notes |
| --- | --- | --- |
| `tiny_model=path` | none | ONNX path for the tiny-AI loader (`0001`). |
| `tiny_device=<ep>` | `auto` | ORT device selector: `auto`, `cpu`, `cuda`, `openvino`, `openvino-npu`, `openvino-cpu`, `openvino-gpu`, `coreml`, `coreml-ane`, `coreml-gpu`, `coreml-cpu`, `rocm` (`0001`). |
| `tiny_threads=N` | `0` | CPU-EP intra-op thread count (`0` = ORT default). |
| `tiny_fp16=0\|1` | `0` | Request fp16 I/O when the device supports it. |
| `sycl_device=N` | `-1` (disabled) | SYCL device index; `-1` keeps the CPU path (`0003`). |
| `sycl_profile=0\|1` | `0` | Enable SYCL queue profiling (`0003`). |
| `cuda=0\|1` | `0` | Enable the CUDA compute path on software-decoded input (`0010`). |
| `hip_device=N` | `-1` (disabled) | HIP device ordinal; `-1` keeps the CPU path (`0011`, [ADR-0380](../adr/0380-ffmpeg-patches-hip-backend-selector.md)). |
| `metal_device=N` | `-2` (disabled) | Metal device index: `-2` disabled, `-1` system default, `>= 0` explicit (`0012`). The `-2` default is a fork-local convention because Metal is auto-disabled on Linux; an unset value must not enable the backend. |
| `perceptual_weight=0\|1` | `0` | Perceptually re-weight VMAF **spatial pooling** using Pelorus side-data (`0017`); see below. |

`sycl_device`, `hip_device` and the selector options error out when libvmaf was
built without the matching backend (`-Denable_sycl=true`, `-Denable_hip=true`).

**`perceptual_weight`.** The Pelorus side-data (per-cell banding-risk and
variance maps) travels on the distorted frame as an unregistered SEI
([ADR-1118](../adr/1118-perceptual-sidedata-weighting.md)). The option is off by
default and golden-isolated: weighting engages only when it is `1` **and** a
valid Pelorus blob is present. Standard scoring, including the Netflix golden
pairs that carry no side-data, is byte-identical to upstream. A foreign or
absent SEI is ignored and the frame is scored unweighted. Pair it with a
Pelorus-enabled producer that emits the interop blob; the C API is in
[`docs/api/perceptual-weight.md`](../api/perceptual-weight.md).

### When to use the `vmaf` CLI instead

- **You need `--precision=max`.** The filter's equivalent is `score_fmt`; the
  CLI is simpler when you want `%.17g` everywhere
  ([ADR-0119](../adr/0119-cli-precision-default-revert.md)).
- **You need the CLI's tiny-AI flags.** `--no-reference` and `--tiny-codec`
  have no filter equivalent, and the filter's tiny-AI options are the four
  `tiny_*` ones above. See [api/dnn.md](../api/dnn.md) for the C-API surface.
- **You want explicit backend control.** `libvmaf_cuda` is chosen by frame
  format inside FFmpeg; the CLI gives `--no_cuda` / `--no_sycl` opt-out
  control.

For everything else, the filter is the right tool.

## Other filters

### `libvmaf_sycl`

The dedicated `libvmaf_sycl` filter (patch `0005`, configure flag
`--enable-libvmaf-sycl`) routes scoring through libvmaf's SYCL backend without
the generic filter's CPU path. It accepts the common options above plus
`gpu_profile=0|1`, which prints a per-kernel GPU timing breakdown at the end.

It has no `sycl_device` option: the device comes from the QSV frame context.
`sycl_device=N` belongs to the generic `libvmaf` filter. See
[vmaf-vpl.md](vmaf-vpl.md) for the SYCL backend overview.

On QSV input the filter imports the **luma plane only**. That is enough for
`vmaf_v0.6.1`, the filter's default model, whose scores on this path equal the
CPU's. A model or feature that needs chroma or a host frame fails on the first
frame. That includes the library's default model `vmaf_v1.0.16_3d0h`, which
reads chroma through `speed_chroma_uv`. A libvmaf error names each feature
extractor that cannot run, and the filter then prints:

> `libvmaf_sycl: the model or a feature needs chroma or host frames, which the
> QSV zero-copy path does not provide. Use hwdownload,format=nv12 and the
> libvmaf filter's sycl_device option.`

The complete list is in
[SYCL zero-copy imports](../backends/sycl/zero-copy.md#zero-copy-import-scores-luma-only-features)
([ADR-1688](../adr/1688-sycl-zero-copy-luma-only-admission.md)).

If importing a decoded surface fails, the filter tries again, three tries in
all with 1 ms between them, and logs each failed try with the frame number. If
the third try fails too, the filter stops with
`libvmaf_sycl: cannot import the reference VA surface <id> of frame <n> after 3 tries`
and prints no pooled score. It never passes a frame through unscored, so a
score always covers every decoded frame
([ADR-1761](../adr/1761-sycl-filter-import-retry-then-fail.md)). Each input's
surfaces are imported with that input's own VA display. The two decoders may
therefore run on two VA devices: their surfaces are no longer read through the
other input's display, which used to give a wrong score without an error.

### `libvmaf_cuda`

`libvmaf_cuda` keeps CUDA hwaccel frames on the GPU: no frame goes through host
memory. It is not zero-copy: the filter copies each decoded frame device to device into
libvmaf's own picture pool ([ADR-1685](../adr/1685-post-1-0-embedding-zero-copy-milestone.md);
importing the decoder's frame without that copy is post-1.0 work). It needs
libvmaf built with `-Denable_cuda=true` and FFmpeg configured with
`--enable-libvmaf-cuda` (patch `0010`). Its frames must be `yuv420p` or
`yuv444p16` on the device, not the `nv12` that NVDEC outputs, so convert with
`scale_cuda=format=yuv420p` first. A frame it cannot copy or read stops it
with no pooled score, as for `libvmaf`
([When a frame cannot be scored](#when-a-frame-cannot-be-scored)).

### `libvmaf_metal`

`libvmaf_metal` (patch `0013`) consumes `AV_PIX_FMT_VIDEOTOOLBOX` frames. See
[GPU filters](#gpu-accelerated-vmaf-through-ffmpeg).

### `libvmaf_tune`

`libvmaf_tune` (patch `0008`,
[ADR-0312](../adr/0312-ffmpeg-patches-vmaf-tune-integration.md))
is a two-input filter that runs alongside a one-pass encode and logs a
recommended CRF for the next pass:

```bash
ffmpeg -i dist.mp4 -i ref.mp4 -lavfi '[0:v][1:v]libvmaf_tune' -f null -
```

Its pads are `main` (distorted) and `reference`, the same order as `libvmaf`.
The log line reads
`recommended_crf=23 (target_vmaf=95.0, observed_vmaf=96.40, n_frames=240)`.

| Option | Default | Effect |
| --- | --- | --- |
| `model` | `version=vmaf_v0.6.1` | Model spec. |
| `feature` | none | Optional `:`-separated feature spec. |
| `n_threads` | `0` | Worker threads for libvmaf (`0` = default; max 1024). |
| `recommend_target_vmaf` | `95.0` | Target VMAF score to recommend a CRF for (0 to 100). |
| `recommend_crf_min` | `18.0` | Lower CRF bound considered (0 to 51). |
| `recommend_crf_max` | `51.0` | Upper CRF bound considered (0 to 51). |
| `recommend_passes` | `1` | Number of probe passes (1 to 8); advisory, ignored by the single-pass implementation. |

The CRF is a piece-wise linear mapping of the observed VMAF onto
`[recommend_crf_min, recommend_crf_max]`. The per-clip search lives in
`tools/vmaf-tune/src/vmaftune/recommend.py`. See
[vmaf-tune-ffmpeg.md](vmaf-tune-ffmpeg.md) for how vmaf-tune uses the filter.

### `vmaf_pre`

`vmaf_pre` (patch `0002`) applies an ONNX learned pre-filter to a video. It
supports 8-bit and 10-bit input and needs libvmaf 3.0.0 or newer built with
DNN support.

| Option | Default | Effect |
| --- | --- | --- |
| `model` | none | Path to the ONNX learned-filter model. |
| `device` | `auto` | Inference device; the same values as `tiny_device`. |
| `threads` | `0` | CPU EP intra-op threads. |
| `chroma` | `0` | Also filter the U and V planes (`0` = luma only). |

### `qpfile` on encoder wrappers

Patch `0007` adds a `qpfile` option to the libx264, SVT-AV1 and libaom wrappers
for a vmaf-tune per-frame QP file. SVT-AV1 needs 1.6.0 or newer, and libaom
quantises the per-macroblock offsets to at most 8 segment QPs. See
[vmaf-tune-ffmpeg.md](vmaf-tune-ffmpeg.md) and
[ADR-0312](../adr/0312-ffmpeg-patches-vmaf-tune-integration.md).

## GPU-accelerated VMAF through FFmpeg

### Per-backend copy-paste examples

One invocation per backend, all on the same input pair (`reference.mp4`,
`distorted.mp4`). The selector options come from patches `0003`, `0010` and
`0011`.

**CPU** (default; no hwaccel and no GPU build needed):

```bash
ffmpeg -i distorted.mp4 -i reference.mp4 \
       -filter_complex "[0:v][1:v]libvmaf=log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

**CUDA, device-resident frames** (NVIDIA only; libvmaf built with
`-Denable_cuda=true`; uses the dedicated `libvmaf_cuda` filter):

```bash
ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i distorted.mp4 \
       -hwaccel cuda -hwaccel_output_format cuda -i reference.mp4 \
       -filter_complex "[0:v]scale_cuda=format=yuv420p[d];[1:v]scale_cuda=format=yuv420p[r];[d][r]libvmaf_cuda=log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

**CUDA, software input** (libvmaf built with `-Denable_cuda=true`, FFmpeg
configured with `--enable-libvmaf-cuda`; the `cuda=1` selector on the regular
`libvmaf` filter runs the CUDA feature kernels without the hwaccel decode
round-trip):

```bash
ffmpeg -i distorted.mp4 -i reference.mp4 \
       -filter_complex "[0:v][1:v]libvmaf=cuda=1:log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

With `cuda=1` the filter creates a `VmafCudaState` on the CUDA primary context
of the default device and imports it into the `VmafContext`. It allocates
`VmafPicture`s from a `HOST_PINNED` pool, so the CUDA kernels DMA from
pinned-host memory without a staging copy. Choose the device with
`CUDA_VISIBLE_DEVICES`: the `VmafCudaConfiguration` C API has no
`device_index` field.

**SYCL** (Intel / Arc; libvmaf built with `-Denable_sycl=true`; the
`sycl_device=N`
selector on the regular `libvmaf` filter, fed by software-decoded frames):

```bash
ffmpeg -i distorted.mp4 -i reference.mp4 \
       -filter_complex "[0:v][1:v]libvmaf=sycl_device=0:log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

!!! note "How the selectors behave"
    In the CPU, SYCL and HIP examples the regular `libvmaf` filter receives
    software-decoded frames, and libvmaf copies them into device memory. Only
    the selector option (`sycl_device=N`, `hip_device=N`) changes the compute
    path. For SYCL, `sycl_device=-1` keeps the CPU path and any non-negative
    ordinal opts in; see
    [backends/sycl/overview.md](../backends/sycl/overview.md)
    for device enumeration.

### Hardware decode plus GPU compute

For long clips or 4K and larger inputs, FFmpeg's hardware decoders are usually
faster than software decode. Bridge them to the libvmaf compute backend:

| Decode | Filter | Needs the `hwdownload,format=yuv420p` bridge? |
| --- | --- | --- |
| CUDA | `libvmaf_cuda` | No |
| QSV / oneVPL | `libvmaf_sycl` | No |
| VideoToolbox | `libvmaf_metal` | No |
| Plain VAAPI (AMD or non-QSV Intel) | `libvmaf` with `sycl_device=N` | Yes |

**CUDA, device-resident** (decode and compute on the same GPU; no host copy, one
device-to-device copy per frame into libvmaf's pool, see
[`libvmaf_cuda`](#libvmaf_cuda)):

```bash
ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i distorted.mp4 \
       -hwaccel cuda -hwaccel_output_format cuda -i reference.mp4 \
       -filter_complex "[0:v]scale_cuda=format=yuv420p[d];[1:v]scale_cuda=format=yuv420p[r];[d][r]libvmaf_cuda=log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

**QSV decode, zero-copy SYCL compute** through `libvmaf_sycl` (no CPU readback;
patch `0005`):

```bash
ffmpeg -hwaccel qsv -hwaccel_output_format qsv -i distorted.mp4 \
       -hwaccel qsv -hwaccel_output_format qsv -i reference.mp4 \
       -filter_complex "[0:v][1:v]libvmaf_sycl=log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

`libvmaf_sycl` consumes oneVPL `mfxFrameSurface1` frames directly
(`AVFrame->data[3]`), extracts the VA surface ID and imports it through
`vmaf_sycl_import_va_surface` for a zero-copy DMA-BUF import on the Level Zero /
SYCL queue. Build FFmpeg with `--enable-libvmaf-sycl` in addition to
`--enable-libvmaf`. The filter consumes oneVPL surfaces specifically, so plain
VAAPI decode still needs the bridge below.

**Plain VAAPI decode, SYCL compute** (software-frame bridge):

```bash
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i distorted.mp4 \
       -hwaccel vaapi -hwaccel_output_format vaapi -i reference.mp4 \
       -filter_complex "[0:v]hwdownload,format=yuv420p[d]; \
                        [1:v]hwdownload,format=yuv420p[r]; \
                        [d][r]libvmaf=sycl_device=0:log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

**VideoToolbox decode, Metal compute** with `libvmaf_metal`:

```bash
ffmpeg -hwaccel videotoolbox -hwaccel_output_format videotoolbox_vld -i distorted.mp4 \
       -hwaccel videotoolbox -hwaccel_output_format videotoolbox_vld -i reference.mp4 \
       -filter_complex "[0:v][1:v]libvmaf_metal=log_fmt=json:log_path=/dev/stdout" \
       -f null -
```

Build FFmpeg with `--enable-libvmaf-metal` against a libvmaf compiled with
`-Denable_metal=enabled`. The filter pulls the `IOSurfaceRef` behind each
`CVPixelBufferRef` through `CVPixelBufferGetIOSurface` and passes it to
`vmaf_metal_picture_import`. That call locks the surface read-only and copies
each plane into a shared-storage `VmafPicture`; on Apple Silicon the
unified-memory cost equals a Shared `MTLBuffer` copy.
`videotoolbox_vld` is FFmpeg's name for VideoToolbox hardware frames;
`-hwaccel_output_format videotoolbox` is not a pixel format name and is
refused.

The decoder must output 4:2:0 frames in NV12 (8-bit) or P010 (10-bit), which
is what VideoToolbox decodes 8-bit and 10-bit 4:2:0 content to. The filter
imports all three planes of both frames. libvmaf splits the interleaved CbCr
plane into Cb and Cr and moves each P010 sample from the top 10 bits of its 16
to the bottom 10 ([ADR-1679](../adr/1679-metal-iosurface-biplanar-import.md)).

The filter checks both inputs when it is configured:

- any other software format stops the filter with an error that names the
  format, for example `libvmaf_metal: VideoToolbox sw_format p210 on the main
  input is not supported (supported: nv12, p010)`. That covers 4:2:2 and
  4:4:4 content such as ProRes or HEVC 4:2:2. Score those with `hwdownload`
  and the `libvmaf` filter's `metal_device` option;
- the two inputs must use the same software format.

If a frame cannot be imported or read, the filter fails with an error that
names the frame. It never passes the frame through unscored, and after the
failure it prints no pooled score and writes no log for the frames before it
(`libvmaf_metal: no pooled score: the filter stopped at frame N`): either
would cover fewer frames than the input
([ADR-1761](../adr/1761-sycl-filter-import-retry-then-fail.md)). VideoToolbox
imports fail for a format or size the surface cannot hold, which a second try
does not change, so the filter does not retry them.

Nothing in this path has run on an Apple device yet. Until a macOS tester
report confirms it, the row `T-METAL-FFMPEG-FILTER-BIPLANAR-IMPORT-2026-10-05`
in [the state ledger](../state.md) stays open.

Two caveats apply:

- On a host without an Apple-Family-7 `MTLDevice`, the filter fails fast at
  `config_props` with `AVERROR(ENODEV)`. See
  [ADR-0423](../adr/0423-metal-iosurface-import-scaffold.md).
- The libvmaf runtime falls back to `MTLCreateSystemDefaultDevice` until
  upstream FFmpeg ships an `AVMetalDeviceContext`. On a multi-GPU Mac Pro this
  may pick a different `MTLDevice` than the VideoToolbox decoder used; a
  same-device contract is a follow-up.

## Model path on Windows

Built-in models need no path: use `model=version=vmaf_v0.6.1`, and skip this
section. It applies only when you pass `model_path` for a model file.

A relative `model_path` needs no escaping. An absolute Windows path such as
`D:\mypath\vmaf_v0.6.1.json` must be escaped so that `ffmpeg` hands the right
path to `libvmaf`. The final command line depends on the shell.

1. Convert every backslash to a forward slash: `D:/mypath/vmaf_v0.6.1.json`.
2. Escape the colon with a backslash: `D\:/mypath/vmaf_v0.6.1.json`.
3. Escape that backslash with another backslash: `D\\:/mypath/vmaf_v0.6.1.json`.
4. Pick the form for your shell.

**PowerShell and Command Prompt.** The path from step 3 is enough:

```powershell
./ffmpeg.exe -i dist.y4m -i ref.y4m \
    -lavfi libvmaf=model_path="D\\:/mypath/vmaf_v0.6.1.json" \
    -f null -
```

Neither shell treats `\` as special, so the quotes around the path are
optional here, and quoting the whole text after `-lavfi` gives the same result.

**bash (msys2).** Two behaviours matter:

- bash treats an unquoted `\` as an escape character, so the escaped backslash
  needs another backslash.
- msys2 converts POSIX-like arguments such as
  `/mingw64/share/model/vmaf_v0.6.1.json` to Windows mixed paths
  (`D:/msys2/mingw64/share/model/vmaf_v0.6.1.json`). Here that conversion
  produces an unescaped colon, so set `MSYS2_ARG_CONV_EXCL` to `*` to turn it
  off for all arguments.

```bash
MSYS2_ARG_CONV_EXCL="*" \
    ./ffmpeg.exe -i dist.y4m -i ref.y4m -lavfi \
    libvmaf=model_path="D\\\:/mypath/vmaf_v0.6.1.json" -f null -
```

Removing the quotes entirely needs four backslashes; quoting the whole argument
is fine. Single quotes around the path also work, with two backslashes instead
of three:

```bash
MSYS2_ARG_CONV_EXCL="*" \
    ./ffmpeg.exe -i dist.y4m -i ref.y4m -lavfi \
    libvmaf=model_path='D\\:/mypath/vmaf_v0.6.1.json' -f null -
```

## External resources

See [external-resources.md](external-resources.md) for a list of FFmpeg-based
third-party tools.

## History

- **Vulkan removed
  ([ADR-0726](../adr/0726-drop-vulkan-backend.md)).** The `libvmaf_vulkan`
  filter, the `vulkan_device=N` option, the `--enable-libvmaf-vulkan` configure
  flag and the `-Denable_vulkan=enabled` build flag no longer exist. Historical
  examples are in git history. Patches `0004` (the selector) and `0006` (the
  filter) stay in the series as no-op shims
  ([ADR-0860](../adr/0860-ffmpeg-patch-chain-no-op-vulkan-shim.md)): they
  compile
  to zero linked code, but keep the context lines that later patches depend
  on.
