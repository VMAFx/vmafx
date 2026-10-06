<!-- markdownlint-disable MD013 MD046 MD060 -->
# Using VMAFx with GStreamer

The `vmafx` element scores a distorted video against a reference inside a
GStreamer pipeline and passes the distorted frames on unchanged. It is built on
the public [VMAFx C API](../api/vmafx/index.md), takes its properties from the
same option table as the [FFmpeg `vmafx` filter](ffmpeg.md), and scores in
place: frames in system memory without a copy, frames in CUDA memory without a
download.

## Build the element {#build}

The element is a standalone Meson project in `gstreamer/`. It needs the
GStreamer development files (base and video libraries, 1.18 or newer) and a
build of the VMAFx library. GStreamer's CUDA library and the CUDA headers are
optional: with them the element also takes CUDA memory.

```bash
meson setup build core -Db_lto=false && ninja -C build
PKG_CONFIG_PATH=$PWD/build/meson-uninstalled meson setup build-gst gstreamer
ninja -C build-gst
export GST_PLUGIN_PATH=$PWD/build-gst LD_LIBRARY_PATH=$PWD/build/src
gst-inspect-1.0 vmafx
```

For a CUDA-capable element, configure the library with
`-Denable_cuda=true -Denable_nvcc=true` and point `meson setup build-gst` at
that build; `-Dcuda=disabled` leaves CUDA memory out, `-Dtoolkit_root=<dir>`
names a CUDA toolkit outside `/opt/cuda`, `/usr/local/cuda` and `/usr`.
`gstreamer/test/run.sh` builds both variants and runs the tests; it exits 77
when the GStreamer development files are missing.

## Score two videos {#first-pipeline}

The element has two sink pads, `reference` and `distorted`, and one source
pad. Pads are named, not positional: link by name, as below. Frames pair in the
order they arrive, so both inputs must deliver the same frames in the same
order.

```bash
gst-launch-1.0 -m \
  filesrc location=ref.yuv  ! rawvideoparse format=i420 width=1920 height=1080 framerate=24/1 ! v.reference \
  filesrc location=dist.yuv ! rawvideoparse format=i420 width=1920 height=1080 framerate=24/1 ! v.distorted \
  vmafx name=v model=version=vmaf_v0.6.1 log-path=score.json ! fakesink
```

`-m` prints the element messages ([below](#messages)); `score.json` is the
same report the `vmaf` CLI writes, with the
[provenance record](provenance.md) embedded. Pass `score-fmt=%.17g` for
lossless numbers: with it, every per-frame and pooled value equals the CLI's
at `--precision max` (`gstreamer/test/test_gst_vmafx_parity.py`).

Without `model`, the element scores with the library's default model
(`vmaf_v1.0.16_3d0h`); upstream's `vmaf` element and the CLI of older releases
default to `vmaf_v0.6.1`, so name it when you compare with their numbers.

### On the GPU {#cuda}

With CUDA memory the element imports the buffers' device pointers: no frame
is copied to the host. Decoders that output CUDA memory link straight to the
pads. For a file, upload first and pin the memory type:

```bash
gst-launch-1.0 -m \
  filesrc location=ref.yuv  ! rawvideoparse format=i420 width=1920 height=1080 framerate=24/1 \
      ! cudaupload ! "video/x-raw(memory:CUDAMemory)" ! v.reference \
  filesrc location=dist.yuv ! rawvideoparse format=i420 width=1920 height=1080 framerate=24/1 \
      ! cudaupload ! "video/x-raw(memory:CUDAMemory)" ! v.distorted \
  vmafx name=v backend=cuda model=version=vmaf_v0.6.1 log-path=score.json ! fakesink
```

`backend=auto` (the default) uses CUDA when the pads carry CUDA memory and the
CPU otherwise. The device is the one of the buffers' CUDA context and stream:
the element orders its work after the producer's stream with an event, keeps
each buffer until the library has read it, and makes the producer's stream
wait for that before the buffer is reused. The CUDA twins of the extractors
that are declared bit-identical to the CPU ones give the CPU's scores; the
library's [exact twins list](../development/cross-backend-exact-twins.md) says
which. The library admits a device frame only when every registered extractor can
read it where it lives; one that cannot refuses it by name, and the element
never downloads a frame to feed it. `import=host` is the explicit request
to download device frames and score on the CPU; the `vmafx-summary` message
counts them in `host-copy-frames`.

NV12 and `P010_10LE` are accepted in system and CUDA memory; the library
de-interleaves them (a copy in system memory, a device pass in CUDA memory).

### Windows, per-frame scores and provenance {#windows}

```bash
... vmafx name=v model=version=vmaf_v0.6.1 pool=min+mean+harmonic_mean \
      n-stats=0.5 stats-out=log+metadata+file stats-path=windows.ndjson \
      metadata=true provenance=log+report log-path=score.json
```

- `n-stats` (seconds) or `n-stats-frames` cut the stream into windows with the
  library's window clock, which cuts on the buffers' timestamps in integer
  nanoseconds (so a window of 0.5 s at 24 fps holds the frames stamped inside
  it, which can be 13 where the stamps round down). The last window is
  `partial` when the stream ends inside it. `stats-out` chooses where each
  window goes: `log` (an info line of the `vmafx` debug category), `metadata`
  (a `vmafx-window` message) and `file` (one JSON object per line in
  `stats-path`, the field names of the FFmpeg filter).
- `metadata=true` posts a `vmafx-frame` message for every scored frame once its
  score is final.
- `provenance=log` posts the `vmafx-provenance` message when scoring starts;
  `provenance=report` embeds the record in the report (JSON and XML always
  carry it; for CSV and SUB it goes to `<log-path>.provenance.json`).

## Messages {#messages}

All are element messages on the bus, posted by the `vmafx` element; fields
named after a model are the model's name (`vmaf` unless `name=` says
otherwise).

| Message | When | Fields |
| --- | --- | --- |
| `vmafx-provenance` | scoring starts (`provenance` has `log`) | `json`: the provenance record |
| `vmafx-frame` | a frame is final (`metadata=true`) | `index`; one `double` per model |
| `vmafx-window` | a window completes (`stats-out` has `metadata`) | `window`, `start`, `end` (seconds), `n_frames`, `n_scored`, `partial`; `<model>.<method>` per model and `pool` method |
| `vmafx-summary` | end of stream | `n_frames`, `host-copy-frames`; `<model>.<method>` pooled over the stream |

A failure posts one error naming the pad (`reference` or `distorted`), the
format and the refusing extractors; no summary follows and no buffer passes
unscored. A layout the element refuses (a format outside its caps, `import=device`
with system memory, CUDA memory with `backend=cpu`) fails when the caps are
negotiated, naming the format.

## Properties {#properties}

Properties are generated from the option table of the library
(`core/api/vmafx.toml`), so a name and its meaning are the FFmpeg filter's with
`_` written `-`. Enumerations and flag sets are strings: `backend=cuda`,
`pool=min+mean`. `n-threads` and `n-subsample` are aliases of `threads` and
`subsample`. Set the properties before the element starts; a value that names
nothing fails the start.

| Property | Type | Default | Meaning |
| --- | --- | --- | --- |
| `model` | string | unset | Model, colon-delimited: version= a built-in model, path= a model file, name= the name in the report, disable_clip, enable_transform, `<feature>.<option>=<value>` overloads. Several models score in one pass (separate them with `\|`). (default: the library default, VMAF_DEFAULT_MODEL_VERSION) |
| `backend` | enum: auto, cpu, cuda, sycl, hip, metal | `auto` | Backend: auto uses the available ones; any other value runs that backend alone and fails when it is not available. |
| `feature` | string | unset | Additional feature extractor, name[=key=value:...] (for example psnr or cambi=full_ref=true); several may be given (separate them with `\|`). Mutually exclusive with the CTC presets. |
| `tiny-model` | string | unset | Tiny ONNX model to load alongside the classic models. |
| `tiny-device` | enum: auto, cpu, cuda, openvino, openvino-npu, openvino-cpu, openvino-gpu, coreml, coreml-ane, coreml-gpu, coreml-cpu, rocm | `auto` | ONNX Runtime execution provider of the tiny model. |
| `tiny-threads` | int64 | `0` | Intra-op threads of the CPU execution provider (0: the runtime's default). |
| `tiny-fp16` | boolean | `false` | Request fp16 input and output where the execution provider supports it. |
| `threads` | int64 | `0` | Worker threads of the feature extractors, capped to the hardware threads (0: score in the calling thread). |
| `n-threads` | int64 | `0` | alias of threads |
| `cpumask` | int64 | `0` | Bitmask of CPU instruction sets the extractors must not use. |
| `gpumask` | int64 | `0` | Bitmask of GPU operations the extractors must not use. |
| `device` | string | `auto` | GPU of the selected backend: auto, or a device index. Needs a GPU backend that selects devices by index (sycl, hip, metal). |
| `subsample` | int64 | `1` | Score every n-th frame (1: every frame). |
| `n-subsample` | int64 | `1` | alias of subsample |
| `log-path` | string | unset | Report file. |
| `log-fmt` | enum: json, xml, csv, sub | `json` | Report format; json and xml carry the backend receipt. |
| `view-distance` | double | `0.0` | Viewing distance in display heights (ADM adm_norm_view_dist). Unset: the model's value. The ADM extractor's default CSF refuses a distance below 3 (it accepts 0.75 with other CSF modes, which these options do not set). |
| `display-height` | int64 | `0` | Height of the reference display in pixels (ADM adm_ref_display_height). Unset: the model's value. |
| `target-width` | int64 | `0` | Width of the target display the distorted video is scaled to (0: no scaling). |
| `target-height` | int64 | `0` | Height of the target display the distorted video is scaled to (0: no scaling). |
| `target-scaling` | enum: none, bilinear, bicubic, lanczos | `none` | Scaling filter towards the target display. |
| `pool` | flags: min, max, mean, harmonic_mean, median, perc5, perc10, perc20 | `mean` | Pool methods of the final score and of the windows. |
| `n-stats` | double | `0.0` | Window length in seconds (0: no windows). |
| `n-stats-frames` | int64 | `0` | Window length in frames (0: no windows); exclusive with n_stats. |
| `stats-out` | flags: log, metadata, file | `log` | Where window statistics go. |
| `stats-path` | string | unset | NDJSON file of the window statistics, one object per window. |
| `score-fmt` | string | `%.6f` | printf format of the scores (%.17g is lossless). |
| `provenance` | flags: log, report | `log+report` | Print the provenance record at init (log) and embed it in the report (report). |
| `metadata` | boolean | `false` | Post a `vmafx-frame` message with the per-frame scores of every model once they are final. |
| `profile` | boolean | `false` | Log the device timing breakdown at uninit. |
| `import` | enum: auto, device, host | `auto` | auto imports hardware frames without a copy and uploads software frames; device refuses software frames; host downloads hardware frames (logged once with the reason). |
| `perceptual-weight` | boolean | `false` | Weight the scores with the perceptual side data of the frames. |

Beyond this table the element carries the properties of GStreamer's aggregator
(`latency`, `min-upstream-latency`, `start-time-selection`, `start-time`,
`emit-signals`, `force-live`).

Notes that differ from the table's text:

- `model` and `feature` take several items separated by `|`
  (`model=version=vmaf_v0.6.1|version=vmaf_4k_v0.6.1:name=vmaf_4k`). Without
  `model` the default model is scored, unless `feature` is set (then only the
  features are).
- `device` is `auto` or a device index; the CPU backend has one device.
- `target-width`, `target-height` and `target-scaling` are refused until
  scoring on a target display lands (RC5); `perceptual-weight` is refused
  because buffers carry no perceptual side data.
- The report and the summary are written at the end of the stream. A pipeline
  that stops early writes neither.

## Moving from upstream's `vmaf` element {#migration}

GStreamer's own `vmaf` element (gst-plugins-bad) reads system memory and calls
the stock `libvmaf`. The `vmafx` element is not a drop-in rename; this is the
mapping.

| Upstream `vmaf` | `vmafx` |
| --- | --- |
| pads `ref_sink`, `dist_sink` | pads `reference`, `distorted` |
| `model-filename` (default `vmaf_v0.6.1`) | `model=version=vmaf_v0.6.1` or `model=path=/path/model.json`; the default model differs |
| `disable-clip`, `enable-transform`, `phone-model` | `model=version=vmaf_v0.6.1:disable_clip` / `:enable_transform` (the phone model is the transform) |
| `psnr`, `ssim`, `ms-ssim` | `feature=psnr\|float_ssim\|float_ms_ssim` |
| `pool-method` (one of min, max, mean, harmonic_mean) | `pool` (a set of up to eight methods; default `mean`) |
| `results-filename`, `results-format` | `log-path`, `log-fmt` (`json`, `xml`, `csv`, `sub`) |
| `frame-message` | `metadata=true` (`vmafx-frame` message) |
| `subsample`, `threads` | the same names (`threads` defaults to 0, scoring on the streaming thread) |
| `log-level` | `GST_DEBUG=vmafx:4` (the library's messages go to the `vmafx` category) |
| `conf-interval` | not available in this release |
| the `samples-selected` signal | none; read the messages |

## Scoring while encoding {#encode-time}

Put the encoder and decoder inside the pipeline and score the decoded frames
against the source, on the GPU, with no host copy:

```bash
gst-launch-1.0 -m filesrc location=src.yuv ! rawvideoparse format=i420 width=1920 height=1080 framerate=24/1 \
  ! videoconvert ! video/x-raw,format=NV12 ! cudaupload ! "video/x-raw(memory:CUDAMemory)" ! tee name=u \
  u. ! queue ! v.reference \
  u. ! queue ! nvh264enc ! h264parse ! nvh264dec ! "video/x-raw(memory:CUDAMemory)" ! queue ! v.distorted \
  vmafx name=v backend=cuda n-stats-frames=24 stats-out=log+metadata ! fakesink
```

`gstreamer/test/test_gst_vmafx_parity.py` runs this on 48 frames and compares
the report and the windows with the same decoded frames scored from files:
bit for bit equal, `host-copy-frames=0`. Encoder timestamps start at a fixed
offset, so the window times are shifted by it and nothing else differs.

## Limits {#limits}

- Backends: CPU and CUDA. `backend=sycl`, `hip` and `metal` fail at start
  naming the backend; each comes with its import lane.
- Memory: system memory and CUDA memory are imported. `GLMemory` (what
  `nvh264dec` offers next to CUDA memory, and VA decoders' second output) and
  `VulkanImage` (`vulkanh264dec`) are in the pad caps so that they negotiate to
  a refusal that names the memory (`pad distorted: format NV12 in GLMemory: not
  imported yet`) instead of a silent download through a converter; each waits
  for its import lane (GL interop on CUDA and SYCL, the Vulkan import memory
  kind). On AMD, negotiate DMABuf instead of GL. VA and DMA-buf, D3D11 and
  Metal-backed memory are not imported yet.
- The two inputs must have the same format and size, and the frames must pair
  one to one; a buffer whose partner never comes is dropped with a warning.
- A window over the default model fails until
  `T-VMAFX-WINDOW-3D0H-NOTFOUND-2026-10-06` ([state](../state.md)) is fixed;
  use `model=version=vmaf_v0.6.1` for windows and `metadata=true`.
- The element scores in the C locale: GStreamer applications set the
  user's locale and the library reads numbers with the calling thread's
  (`T-VMAFX-OPTION-PARSE-LOCALE-2026-10-06`).
- The plug-in registers the licence string `EUPL-1.2`, which is not in
  GStreamer's list of known licences: it loads, and `GST_DEBUG=GST_PLUGIN_LOADING:2`
  shows one warning.
