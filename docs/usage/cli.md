<!-- markdownlint-disable MD033 MD060 -->
# `vmaf` — command-line reference

`vmaf` scores a reference / distorted video pair with one or more VMAF models
(plus any extra feature extractors) and writes per-frame and pooled scores to
an XML, JSON, CSV or subtitle log. This page is the canonical flag reference:
every flag in `vmaf --help`, with its default, interactions and an example.

!!! note "Scope"
    The code's `--help` is authoritative for the *set* of flags at any given
    commit. This page adds defaults, interactions and runnable examples per
    [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md). It supersedes
    the abbreviated help string in
    [`core/tools/README.md`](../../core/tools/README.md).

Related pages: [bench.md](bench.md) for the `vmaf_bench` micro-benchmark,
[ffmpeg.md](ffmpeg.md) for the FFmpeg `libvmaf` filter,
[python.md](python.md) for the Python bindings and
[vmafx-cli.md](vmafx-cli.md) for the `vmafx` alias.

## Quick start

Build the binary as described in the
[source-build guide](../getting-started/index.md#build-from-source-any-platform);
with `meson setup build core` it lands at `build/tools/vmaf`.

```shell
# .y4m pair: no geometry flags needed
vmaf --reference ref.y4m --distorted dist.y4m

# .yuv pair: geometry is mandatory
vmaf \
  --reference ref.yuv \
  --distorted dist.yuv \
  --width 1920 --height 1080 \
  --pixel_format 420 --bitdepth 8 \
  --output scores.xml
```

Without `--model`, the built-in `vmaf_v1.0.16_3d0h` model is loaded. Without
`--xml|--json|--csv|--sub`, the output format is XML.

!!! warning "The default model differs from upstream Netflix"
    Upstream Netflix defaults to `vmaf_v0.6.1`; this fork defaults to
    `vmaf_v1.0.16_3d0h` ([ADR-1169](../adr/1169-default-model-v1-0-16.md)), so
    the same command prints different numbers on each. Pin
    `--model version=vmaf_v0.6.1` to get the upstream values. On the standard
    576x324 test pair the pooled VMAF moves from `76.667831` to `82.816060`.
    See [models/v1.md](../models/v1.md) for the v1 model family.

## Option reference

Every option of `vmaf`, generated from the API definition
(`core/api/vmafx.toml`, option groups; see
[API generation](../development/api-generation.md#option-groups)). The
sections below explain each group in detail. `vmaf --help` prints the same
list from the same source.

<!-- BEGIN GENERATED: vmafx-api cli options (scripts/codegen/vmafx-api.py) -->

| Option | Short | Value | Default | Description |
| --- | --- | --- | --- | --- |
| `--reference` | `-r` | string | | Reference video: a .y4m file, or raw planar .yuv together with the width, height, pixel format and bit depth. |
| `--distorted` | `-d` | string | | Distorted video, in the same form as the reference. |
| `--width` | `-w` | uint >= 1 | | Width of raw .yuv input in pixels. |
| `--height` | `-h` | uint >= 1 | | Height of raw .yuv input in pixels. |
| `--pixel_format` | `-p` | `420` \| `422` \| `444` | | Chroma subsampling of raw .yuv input. |
| `--bitdepth` | `-b` | `8` \| `10` \| `12` \| `16` | | Bits per sample of raw .yuv input. |
| `--check-sample-range`, `--check_sample_range` | | bool | `false` | Refuse a frame with a sample above 2^bpc - 1 and name the plane, row, column and value (ADR-1918). |
| `--color_range_ref` | | `unknown` \| `limited` \| `full` | | Colour range of the reference input. |
| `--color_range_dist` | | `unknown` \| `limited` \| `full` | | Colour range of the distorted input. |
| `--color_primaries_ref` | | `unknown` \| `bt709` \| `bt2020` | | Colour primaries of the reference input. |
| `--color_primaries_dist` | | `unknown` \| `bt709` \| `bt2020` | | Colour primaries of the distorted input. |
| `--color_trc_ref` | | `unknown` \| `bt709` \| `smpte2084` \| `pq` | | Transfer characteristic of the reference input. |
| `--color_trc_dist` | | `unknown` \| `bt709` \| `smpte2084` \| `pq` | | Transfer characteristic of the distorted input. |
| `--color_matrix_ref` | | `unknown` \| `bt709` \| `bt2020nc` \| `ictcp` | | Matrix coefficients of the reference input. |
| `--color_matrix_dist` | | `unknown` \| `bt709` \| `bt2020nc` \| `ictcp` | | Matrix coefficients of the distorted input. |
| `--model` | `-m` | string | library default (`VMAF_DEFAULT_MODEL_VERSION`) | Model, colon-delimited: version= a built-in model, path= a model file, name= the name in the report, disable_clip, enable_transform, &lt;feature&gt;.&lt;option&gt;=&lt;value&gt; overloads. Several models score in one pass (repeat the option; the filter separates them with \|). |
| `--backend` | | `auto` \| `cpu` \| `cuda` \| `sycl` \| `hip` \| `metal` | `auto` | Backend: auto uses the available ones; any other value runs that backend alone and fails when it is not available. |
| `--no_cuda` | | bool | `false` | Disable the CUDA backend. |
| `--no_sycl` | | bool | `false` | Disable the SYCL (oneAPI) backend. |
| `--no_hip` | | bool | `false` | Disable the HIP (ROCm) backend. |
| `--no_metal` | | bool | `false` | Disable the Metal (Apple silicon) backend. |
| `--feature` | | string | | Additional feature extractor, name[=key=value:...] (for example psnr or cambi=full_ref=true); several may be given (the filter separates them with \|). Mutually exclusive with the CTC presets. |
| `--aom_ctc` | | `v1.0` \| `v2.0` \| `v3.0` \| `v4.0` \| `v5.0` \| `v6.0` \| `v7.0` | | AOM common test conditions preset: a fixed model and feature set. |
| `--nflx_ctc` | | `v1.0` | | Netflix common test conditions preset: a fixed model and feature set. |
| `--tiny-model`, `--tiny_model` | | string | | Tiny ONNX model to load alongside the classic models. |
| `--tiny-device`, `--tiny_device` | | `auto` \| `cpu` \| `cuda` \| `openvino` \| `openvino-npu` \| `openvino-cpu` \| `openvino-gpu` \| `coreml` \| `coreml-ane` \| `coreml-gpu` \| `coreml-cpu` \| `rocm` | `auto` | ONNX Runtime execution provider of the tiny model. |
| `--dnn-ep`, `--dnn_ep` | | `auto` \| `cpu` \| `cuda` \| `openvino` \| `openvino-npu` \| `openvino-cpu` \| `openvino-gpu` \| `coreml` \| `coreml-ane` \| `coreml-gpu` \| `coreml-cpu` \| `rocm` | | Alias of the tiny-model device under the ONNX Runtime name (execution provider); both set the same setting. |
| `--tiny-threads`, `--tiny_threads` | | uint | | Intra-op threads of the CPU execution provider (0: the runtime's default). |
| `--tiny-fp16`, `--tiny_fp16` | | bool | `false` | Request fp16 input and output where the execution provider supports it. |
| `--tiny-model-verify`, `--tiny_model_verify` | | bool | `false` | Require a Sigstore bundle verification of the tiny model (cosign verify-blob) before it loads; a missing bundle, a missing cosign or a failed verification refuses the model. |
| `--tiny-codec`, `--tiny_codec` | | string | | Encoder of the distorted clip, required by codec-aware tiny models (fr_regressor_v2/v3), which refuse to score without it. Must be in the model sidecar's encoder_vocab; the ffprobe names h264, hevc, av1, vp9 and vvc are accepted. |
| `--tiny-preset`, `--tiny_preset` | | string | | Encoder preset (medium, slow, p4, 5, ...), read as the encoder defines it. Unset: ordinal 5 (medium). A model trained with one preset (fr_regressor_v3) ignores it and warns. |
| `--tiny-crf`, `--tiny_crf` | | uint 0..63 | | CRF or QP used for the encode, normalised as the model sidecar declares. Required with the codec and preset. |
| `--tiny-resize`, `--tiny_resize` | | `bilinear` \| `nearest` \| `bicubic` \| `disabled` | `disabled` | Resize filter for NCHW tiny models whose input size differs from the frame; disabled refuses the mismatch (-ERANGE). The three filters give scores about 2% apart: record the filter with the model. |
| `--no-reference`, `--no_reference` | | bool | `false` | No-reference mode; needs a no-reference tiny model. The reference becomes a formality: only the distorted picture is scored. |
| `--threads` | | uint | | Worker threads of the feature extractors, capped to the hardware threads (0: score in the calling thread). |
| `--frame_cnt` | | uint >= 1 | | Score at most this many frames. |
| `--frame_skip_ref` | | uint | | Skip this many frames at the start of the reference. |
| `--frame_skip_dist` | | uint | | Skip this many frames at the start of the distorted video. |
| `--no_prediction` | `-n` | bool | `false` | Extract features only; no model score. |
| `--cpumask` | `-c` | uint | | Bitmask of CPU instruction sets the extractors must not use. |
| `--gpumask` | | uint | | Bitmask of GPU operations the extractors must not use. |
| `--sycl_device` | | uint | | SYCL GPU by index (unset: selected automatically). |
| `--hip_device` | | uint | | HIP GPU by index (opt-in: HIP is off unless this or --backend hip is given). |
| `--metal_device` | | uint | | Metal GPU by index (opt-in: Metal is off unless this or --backend metal is given). |
| `--subsample` | | uint >= 1 | `1` | Score every n-th frame (1: every frame). |
| `--precision` | | `legacy` \| `max` \| `full` \| `1` \| `2` \| `3` \| `4` \| `5` \| `6` \| `7` \| `8` \| `9` \| `10` \| `11` \| `12` \| `13` \| `14` \| `15` \| `16` \| `17` | `legacy` | Score precision: N (1 to 17) writes %.&lt;N&gt;g; max or full write %.17g (round-trip lossless); legacy writes %.6f (Netflix-compatible). The scoring server returns lossless scores unless asked otherwise. |
| `--output` | `-o` | string | | Report file. |
| `--xml`, `--json`, `--csv`, `--sub` | | `json` \| `xml` \| `csv` \| `sub` | `xml` | Report format; json and xml carry the backend receipt. |
| `--provenance-sidecar` | | bool | `false` | Also write the provenance record to &lt;output&gt;.provenance.json; CSV and SUB reports carry it nowhere else (RC4 WP5). |
| `--netflix-compat`, `--netflix_compat` | | bool | `false` | Restore the Netflix-upstream defaults: CPU backend, %.6f precision, vmaf_v0.6.1 model. |
| `--quiet` | `-q` | bool | `false` | Disable the FPS meter when run in a terminal. |
| `--version` | `-v` | bool | `false` | Print the version and exit. |
| `--list-backends` | | bool | `false` | Print the scoring backends this binary was built with and which of them initialise here, as JSON, and exit (ADR-1874). |
| `--help` | | bool | `false` | Print this message and exit. |
| `--verify-provenance` | | string | | Re-run the configuration a JSON report records and compare every configuration field and score bit for bit; exits nonzero naming the first difference (RC4 WP5). |

<!-- END GENERATED: vmafx-api cli options -->

## Input flags

| Flag | Short | Argument | Required | Notes |
| --- | --- | --- | --- | --- |
| `--reference` | `-r` | path | **yes** (not with `--no-reference`) | `.y4m` or `.yuv` path. |
| `--distorted` | `-d` | path | **yes** | `.y4m` or `.yuv` path. |
| `--width` | `-w` | unsigned | **yes for `.yuv`** | Ignored for `.y4m` (embedded). |
| `--height` | `-h` | unsigned | **yes for `.yuv`** | Ignored for `.y4m`. |
| `--pixel_format` | `-p` | `420` \| `422` \| `444` | **yes for `.yuv`** | 420 covers the overwhelming majority of streamable content. |
| `--bitdepth` | `-b` | `8` \| `10` \| `12` \| `16` | **yes for `.yuv`** | 10 and 12 bit require a 10-/12-bit aware model (e.g. `vmaf_b_v0.6.3` for banding sensitivity). |
| `--check-sample-range` | | none | no | Refuse a frame with a sample above 2^bitdepth - 1 (for example 1024 in a 10-bit `.yuv`) and stop with a non-zero exit status; the message names the picture, plane, row, column and value. Off by default. Underscore alias `--check_sample_range`. See [Sample range](../api/sample-range.md). |

If any of `--width`, `--height`, `--pixel_format`, `--bitdepth` is supplied,
the input is treated as raw YUV and **all four** become mandatory. `vmaf` reads
only `.y4m` and `.yuv`; decode other containers first.

A raw `.yuv` file of 10 or 12 bits stores each sample in 16 bits, so it can
hold values above 2^bitdepth - 1. Such input is invalid: the CPU extractors and
their GPU twins may score it differently. `--check-sample-range` finds the
first such sample.

Odd frame dimensions (for example 1921x1081 or 19x19) are accepted for raw
`.yuv` and `.y4m` inputs in 4:2:0 and 4:2:2. Chroma plane extents use ceiling
division (`(dim + 1) / 2`), which matches container layouts and covers the last
boundary samples
([ADR-1398](../adr/1398-cli-accept-odd-dimensions-chroma-subsampled.md)).

## Models

`--model` / `-m` takes a colon-delimited `key=value` string (see
[Option-string grammar](#option-string-grammar) for escaping `:` and `\`):

```text
--model path=<file>         # load a .json model from disk
--model version=<builtin>   # load a built-in model by name
--model path=...:name=<str> # rename the metric in the output log
--model version=...:disable_clip          # disable score clipping to [0, 100]
--model version=...:enable_transform      # apply transform
```

Pass `--model` several times to run several models in one pass. Each model
needs a unique `name=`, otherwise the CLI errors out. This example runs VMAF
and VMAF-NEG side by side:

```shell
vmaf -r ref.y4m -d dist.y4m \
  --model version=vmaf_v0.6.1:name=vmaf \
  --model version=vmaf_v0.6.1neg:name=vmaf_neg \
  --output scores.json --json
```

### Built-in model versions

Built-in models are compiled into `libvmaf` with `-Dbuilt_in_models=true`
(the default).

| Version | Purpose |
| --- | --- |
| `vmaf_v1.0.16_3d0h` | **Default.** v1.0.16 standard 1080p model, 3H viewing distance. |
| `vmaf_v1.0.16_1d5h_2160` | v1.0.16 4K model, 2160p at 1.5H. Used by the fork's 4K resolution ladder. |
| `vmaf_v1.0.16_5d0h` | v1.0.16 phone model (1080p at 5H). |
| `vmaf_v1.0.16_3d0h_2160` | v1.0.16 consumer 4K (2160p at 3H); operates on a [0, 110] range. |
| `vmaf_v1.0.16_hfr_3d0h`, `vmaf_v1.0.16_hfr_1d5h_2160`, `vmaf_v1.0.16_hfr_5d0h`, `vmaf_v1.0.16_hfr_3d0h_2160` | The four v1.0.16 models for high-frame-rate content (about 50 / 60 fps), one per viewing condition above. Their motion feature uses the five-frame window with a moving average ([VMAF v1 models](../models/v1.md#high-frame-rate-hfr-content), [Motion](../metrics/motion.md#five-frame-window)). |
| `vmaf_v0.6.1` | Previous default, and still upstream's. 1080p training set, classic release. |
| `vmaf_v0.6.1neg` | Negative-gain (NEG), non-enhancing; recommended for encoder A/B where one encoder may artificially sharpen. There is no NEG counterpart to any v1.0.16 model, so asking for NEG also selects the v0.6.1 generation. |
| `vmaf_b_v0.6.3` | Banding-aware variant (used with CAMBI). |
| `vmaf_4k_v0.6.1` | 4K training set. |
| `vmaf_4k_v0.6.1neg` | 4K + NEG. |

The float-precision variants (`vmaf_float_v0.6.1`, `vmaf_float_v0.6.1neg`,
`vmaf_float_b_v0.6.3`, `vmaf_float_4k_v0.6.1`) also resolve but are legacy.
Prefer the integer versions for performance; use the float versions only for
bit-exact comparison with older reports.

### Model file limits

JSON model files may contain at most **512 simultaneously nested arrays or
objects**, including the outermost container. A 513th level fails model
loading; the internal parser records `maximum depth of nesting reached`, and
the CLI may report only the enclosing model-load failure.

The limit does not cap the number of features or array elements at one level.
Flatten unnecessarily nested custom model data instead of nesting deeper.

## Additional features

`--feature` enables extra metrics beyond what the model already consumes. The
syntax is the same colon-delimited form as `--model`, with the same escaping
rules ([Option-string grammar](#option-string-grammar)):

```shell
--feature psnr
--feature psnr=enable_chroma=true:enable_apsnr=true
--feature float_ssim=enable_db=true:clip_db=true
--feature cambi
--feature ciede
--feature psnr_hvs
--feature brisque
--feature brisque=model_path=/path/to/brisque_live.model
--feature motion=motion_five_frame_window=true:motion_moving_average=true
```

See [../metrics/features.md](../metrics/features.md) for the full list of
feature identifiers and per-feature options.

Option validation is strict. An unknown option key or a typo (for example
`--feature adm=adm_csf_moed=2`) is rejected immediately with
`libvmaf ERROR feature extractor '<name>': unknown option '<key>'`, and `vmaf`
exits non-zero
([ADR-1183](../adr/1183-model-options-gate-gpu-twin-selection.md)).

### Feature-specific notes

- **`brisque`.** The no-reference metric ships its trained model embedded in
  the binary, so it needs no extra arguments. `model_path` overrides it with an
  on-disk libsvm model and is required only for builds with `built_in_models`
  disabled. See [../metrics/brisque.md](../metrics/brisque.md).
- **Five-frame motion window.** The last example above is the motion feature
  of the high-frame-rate models on its own: each frame is compared with the
  frame two back instead of the previous one
  ([Motion, five-frame window](../metrics/motion.md#five-frame-window)). Its
  scores are written under `integer_motion2_mffw_mma` and
  `integer_motion3_mffw_mma`.
- **Picture pool with that window.** The context keeps the reference pictures
  of the two frames before the current one, so its picture pool needs at least
  four pictures. The tool sizes its pool before it loads the models, so a run
  without `--threads` always preallocates four, one more than it needs without
  the window; with `--threads` the count is unchanged. The
  `picture pool: N pictures pre-allocated` line counts them, read-ahead
  pictures included. A C API caller that preallocates fewer than four pictures
  for an `_hfr` model gets `-EINVAL` instead of a stall ([C
  API](../api/index.md)).

### Shared feature contexts

Repeated registrations share work only when their option-derived feature keys
match:

- `--feature motion` and `--feature motion=motion_force_zero=true` keep
  separate contexts and output keys; the latter adds the `_force_0` suffix.
- Explicit default values and option aliases resolve to the same keys as their
  canonical settings.
- Equivalent CPU/GPU twins keep the first registered context.
- The same rules apply when several loaded models request different feature
  parameters.

### Feature extractors on a GPU backend

With `--backend cuda`, `sycl`, `hip` or `metal`, a `--feature` that names a CPU
extractor runs on that backend's twin, picked the same way a model's features
are ([ADR-1359](../adr/1359-cli-feature-backend-twin.md)):

```shell
vmaf ... --backend sycl --feature ciede          # runs ciede_sycl
vmaf ... --backend cuda --feature float_ssim     # runs float_ssim_cuda
vmaf ... --backend sycl --feature brisque        # no twin: runs brisque on the CPU
```

The twin is used only when it can compute exactly what you asked for. In the
four cases below the CPU extractor runs instead, and `vmaf` prints one warning
line on stderr naming the feature and the reason:

| Reason | Warning (for `--backend sycl`) |
| --- | --- |
| The backend has no twin of the extractor | `vmaf: warning: --feature brisque: the sycl backend has no twin of this extractor; computing it on the CPU` |
| The twin lacks an option you set, or implements only its default value | `vmaf: warning: --feature float_motion: float_motion_sycl cannot honour option 'motion_filter_size'; computing it on the CPU` |
| The twin cannot run this input size and bit depth with these options | `vmaf: warning: --feature float_ssim: float_ssim_sycl cannot run 100x100 8-bit pictures with these options; computing it on the CPU` (with `float_ssim=scale=10`, which leaves less than SSIM's 11x11 window) |
| A non-zero `--gpumask` disables the backend's extractors | `vmaf: warning: --feature ciede: cuda feature extraction is disabled (non-zero --gpumask); computing it on the CPU` |

The run carries on after a warning; the
[JSON receipt](#backend-receipt-in-json-output) lists which extractor ran
where. A twin computes the same metric names, and its scores can differ from
the CPU extractor's within the
[cross-backend tolerances](../development/cross-backend-gate.md). Its
auxiliary outputs can differ too:

- On SYCL, `--feature motion` reports `integer_motion`, `integer_motion2` and
  `integer_motion3`, where the CPU extractor reports
  `VMAF_integer_feature_motion_sad_score` instead of `integer_motion`.
- On SYCL, `--feature float_motion` reports `motion` and `motion2` but not
  `motion3`, which only the CPU extractor computes.

Use `--backend cpu` to get the CPU extractor's exact output.

Nothing changes in three cases:

- **A twin name**, such as `--feature ciede_sycl`, registers that extractor.
  It still fails with exit code `100` when its backend is not active
  ([ADR-0543](../adr/0543-adr-0498-enforcement-hardening.md)).
- **`--backend cpu`** always runs the CPU extractor.
- **`--backend auto`, or no `--backend`**, registers the name as given, even
  when a GPU backend is initialised.

Features that a `--model` needs resolve to the active backend's twin in every
mode, with the same option and input-size checks.

## Option-string grammar

`--model` and `--feature` take a **colon-delimited list of `key=value`
pairs**:

- `:` separates pairs.
- The *first* `=` in a pair separates the key from the value; everything after
  it belongs to the value, so a path may contain further `=` characters.
- `.` separates the feature name from the option name in a model feature
  overload (`--model version=...:adm.adm_enhn_gain_limit=1.2`).

Keys and values escape differently, because values are where paths live:

| You want | In a key | In a value |
| --- | --- | --- |
| a literal `:` | `\:` | `\:` |
| a literal `=` | `\=` | `=` or `\=` |
| a literal `.` | `\.` (overload feature/option name) | `.` |
| a literal `\` | `\\` | `\` (but see below) |

A *key* is the part before the first `=`, the feature name of `--feature`, or
either half of a model overload key. In a key, `\:`, `\=`, `\.` and `\\` are
escapes and any other backslash is data.

### Backslashes in values

In a *value*, a backslash is data, so Windows paths are written as they are:

```shell
vmaf -r ref.y4m -d dist.y4m --model 'path=C:\models\vmaf_v0.6.1.json'
vmaf -r ref.y4m -d dist.y4m --model 'path=..\..\models\vmaf_v0.6.1.json'
vmaf -r ref.y4m -d dist.y4m --model 'path=\\server\share\models\vmaf_v0.6.1.json'
vmaf -r ref.y4m -d dist.y4m --model 'path=C:\models\.cache\vmaf_v0.6.1.json'
vmaf -r ref.y4m -d dist.y4m --model 'path=/srv/models=2026/vmaf.json'
vmaf -r ref.y4m -d dist.y4m --model 'path=/srv/odd\:name/vmaf.json'
```

The one exception is a run of backslashes directly before a `:` or `=`, or at
the end of the value. Such a run is read in pairs: `\\` stands for one
backslash, and a single backslash left over escapes the `:` or `=` after it (at
the very end of the value it stays a backslash). This keeps a backslash in
front of a delimiter expressible:

| Written | Value |
| --- | --- |
| `path=C:\out\` | `C:\out\` |
| `path=C:\out\\:name=x` | `C:\out\`, then the pair `name=x` |
| `name=a\:b` | `a:b` |
| `name=a\\\:b` | `a\:b` |

A `:` that spells a Windows drive letter is treated as data, not as a pair
separator. That means a single ASCII letter at the start of a key or a value,
followed by `:` and then `\` or `/`.

!!! warning "Quote the whole option string"
    Use single quotes in your shell, as above. Otherwise the shell eats the
    backslashes before `vmaf` sees them.

The rules are identical for `--model`, `--feature` and the `vmafx` alias.

## Output

| Flag | Default | Notes |
| --- | --- | --- |
| `--output` / `-o <path>` | no file | Writes the per-frame + pooled log to `<path>`. |
| `--xml` | **default** | XML report (upstream-compatible). |
| `--json` | | JSON report. |
| `--csv` | | One row per frame. |
| `--sub` | | SubRip subtitle format, useful for overlaying scores during playback. |

Progress and the per-model pooled score are written to stderr only when stderr
is a terminal. The progress line is suppressed by `--quiet`; the pooled score
line is suppressed only when `--quiet` and `--output` are both given. When
stderr is redirected, nothing is printed there.

Each `<metric>` row of XML, and each entry of `pooled_metrics` in JSON, carries
`min`, `max`, `mean` and `harmonic_mean`; there is no flag to select a subset.
The percentile methods (`median`, `perc5`, `perc10`,
`perc20`) are available through the C API
([ADR-1188](../adr/1188-percentile-pooling-methods.md)) but are not written
into the report.

### Backend receipt in JSON output

A JSON report ends with the score format, the provenance record of the run and
two keys that say where the features were computed
([ADR-1359](../adr/1359-cli-feature-backend-twin.md)); the library writes all
of them ([ADR-2073](../adr/2073-vmafx-provenance-record.md)):

```json
"score_format": "%.17g",
"provenance": {"abi_major":0,"abi_minor":1,"abi_patch":5,"active_backend":"sycl", ...},
"feature_backends": [
  {"extractor": "ciede_sycl", "backend": "sycl"},
  {"extractor": "brisque", "backend": "cpu"}
],
"backend_used": "sycl"
```

- **`backend_used`** is `cuda`, `sycl`, `hip` or `metal` when at least one
  feature extractor ran on that device, and `cpu` when every extractor ran on
  the CPU. It reports what ran, not what was initialised: `--backend sycl` with
  only `--feature brisque` and `--no_prediction` reports `cpu`.
- **`feature_backends`** lists every feature extractor of the run, in
  registration order, with the backend it ran on. It includes the extractors a
  model needs and reflects the CPU replacements made after the first frame, so
  a model feature that fell back to the CPU appears as a `cpu` entry.
- **`provenance`** is the library's provenance record of the run: library,
  ABI and build, device, options, frames, models with their SHA-256, the
  producer of every feature, the command line, and the digests that let
  `--verify-provenance` check the report. [Score provenance](provenance.md)
  documents every field. The scoring server copies it into every response
  ([scoring API contract](../server/api-contract.md)).
- **`score_format`** is the printf format the scores were written with.

A run mixed device twins and CPU extractors when `backend_used` names a device
and `feature_backends` holds at least one `cpu` entry. XML reports carry the
record as a `<provenance>` element; CSV and SUB reports carry none of these
keys, and `--provenance-sidecar` writes the record next to them as
`<output>.provenance.json`.

### Verifying a report

```bash
vmaf --verify-provenance report.json
```

re-runs the command line a JSON report recorded and compares every
configuration field and every score bit for bit; it exits 0 on a match, 1
naming the first difference, and 2 when the check cannot run. See
[Score provenance](provenance.md#quick-start).

### Score precision

```text
--precision N          # printf "%.<N>g", N in 1..17
--precision max|full   # printf "%.17g": IEEE-754 round-trip lossless (opt-in)
--precision legacy     # printf "%.6f": synonym for the default
```

The default is `%.6f`, which matches upstream Netflix output byte for byte so
the CPU golden gate passes without explicit flags
([ADR-0119](../adr/0119-cli-precision-default-revert.md), which supersedes
[ADR-0006](../adr/0006-cli-precision-17g-default.md)). Pass `--precision=max`
whenever you need IEEE-754 round-trip lossless output: cross-backend numeric
diffs, archival reports, any consumer that re-parses scores into doubles. The
flag affects XML, JSON, CSV, SUB and stderr consistently.

The `vmafx` alias defaults to `--precision=max`
([vmafx-cli.md](vmafx-cli.md)). See [precision.md](precision.md) for when to
pick each mode.

## Backend selection

Each backend is opt-in at build time through a meson flag. At **runtime**,
backend selection is per invocation through the flags below; no environment
variable overrides it. If no GPU backend is built in, the GPU flags are
silently inert. See [../backends/index.md](../backends/index.md) for the
runtime dispatch rules and which features have GPU or SIMD twins.

### Selection flags

| Flag | Default | Effect |
| --- | --- | --- |
| `--backend <name>` | `auto` | Exclusive backend selector: `auto`, `cpu`, `cuda`, `sycl`, `hip`, `metal`. A specific backend disables the others through the matching `--no_X` flags before dispatch and pins its device index: `gpumask=0` for CUDA, `sycl_device=0`, `hip_device=0` or `metal_device=0` for the others. With `auto`, the compiled-in backends compete by registry order (SYCL, then CUDA); HIP and Metal join only through their device flags. |
| `--no_cuda` | off | Forbid CUDA dispatch even if the CUDA backend is built in. |
| `--no_sycl` | off | Forbid SYCL dispatch even if the SYCL backend is built in. |
| `--sycl_device <N>` | auto (first GPU) | Pick the SYCL device by ordinal from the oneAPI device list. |
| `--no_hip` | off | Forbid HIP/ROCm dispatch even if the HIP backend is built in. |
| `--hip_device <N>` | disabled (opt-in) | Pick the HIP/ROCm device by ordinal; `0` is the first AMD GPU. Without this flag the HIP backend is never used, even when built with `-Denable_hip=true`. See [../backends/hip/overview.md](../backends/hip/overview.md). |
| `--no_metal` | off | Forbid Metal dispatch even if the Metal backend is built in (macOS only). |
| `--metal_device <N>` | disabled (opt-in) | Pick the Metal GPU by ordinal (macOS only); `0` is the first Metal device, typically the integrated Apple GPU on Apple Silicon. Without this flag the Metal backend is never used, even on macOS builds. See [../backends/metal/index.md](../backends/metal/index.md). |
| `--cpumask <bitmask>` (`-c`) | all ISAs enabled | Mask out specific CPU ISAs (for example force scalar, or disable AVX-512). Values are fork-internal; see `core/src/cpu.h`. |
| `--gpumask <mask>` | GPU enabled | Not a per-op mask; see [`--gpumask`](#the-gpumask-flag). |
| `--threads <N>` | `0` (serial) | Worker thread count; see [Threads](#threads). |

### Which backends this binary can use

`vmaf --list-backends` prints, as one JSON document on stdout, every backend
the CLI knows (`cpu`, `cuda`, `sycl`, `hip`, `metal`, in that order) and exits
0. It needs no input files and ignores the other options.

```json
{
  "backends": [
    {"name": "cpu", "compiled": true, "usable": true},
    {"name": "cuda", "compiled": true, "usable": false, "init_status": -19},
    {"name": "sycl", "compiled": false, "usable": false},
    {"name": "hip", "compiled": false, "usable": false},
    {"name": "metal", "compiled": false, "usable": false}
  ]
}
```

| Field | Meaning |
| --- | --- |
| `compiled` | The backend is built into this binary. The `--help` text lists every backend name on every build, so it does not answer this. |
| `usable` | The backend's state initialises on its default device (index 0) on this host: the same call a run with `--backend <name>` makes. Always `true` for `cpu`, never for a backend that is not compiled. |
| `init_status` | Present when a compiled backend did not initialise: the negative errno its initialiser returned (for example `-19`, `-ENODEV`, when no device is found). |

Each compiled GPU backend is initialised once and released, which can take up
to about a second per backend. The library may log why an initialisation
failed on stderr; stdout carries only the JSON. `vmaf-tune --score-backend`
and `vmafx-tune --score-backend` read this report
([score backends](vmaf-tune-score-backend.md),
[ADR-1874](../adr/1874-vmaf-list-backends.md)).

### The gpumask flag

Despite the `$bitmask` placeholder in the usage string, `--gpumask` is not a
per-operation mask:

- Passing the flag at all opts into GPU backend selection.
- Any non-zero value then disables the GPU feature extractors for both CUDA and
  SYCL, so the run falls back to the CPU implementations.
- `--gpumask 0` therefore means "use the GPU" and `--gpumask 1` means "use the
  CPU"; the latter is byte-identical to `--no_cuda --no_sycl`.
- Negative values are rejected (`should be a non-negative integer`). Upstream
  accepts `--gpumask -1` only because POSIX `strtoul` silently converts `"-1"`
  to `ULONG_MAX`; this fork refuses a value the caller did not mean
  ([ADR-1209](../adr/1209-cli-gpumask-negative-contract.md)). Write
  `--gpumask 1` instead.

### Threads

Without `--threads`, no worker pool is created and CPU extractors run one frame
at a time. Values above the host's core count are capped. The flag is valid
with every backend, including `cuda` and `sycl`, and the result is identical to
a serial run.

Threaded CPU submission keeps at most one pending frame job per worker, in
addition to jobs already running. When decoding runs ahead of feature
extraction, submission waits for queue capacity instead of retaining an
unbounded backlog. This bounds pending work; score storage and backend buffers
still contribute to memory use. See
[thread-pool behavior](../development/thread-pool.md).

### Removed backends

!!! note "Vulkan backend removed (ADR-0726)"
    The `--no_vulkan`, `--vulkan_device` and `--backend vulkan` flags no longer
    exist; passing them produces an unrecognised-option error. See
    [../backends/vulkan/overview.md](../backends/vulkan/overview.md) for
    historical context.

## Frame range

```text
--frame_cnt <N>           # stop after N frames (both streams)
--frame_skip_ref <N>      # skip the first N frames of the reference
--frame_skip_dist <N>     # skip the first N frames of the distorted
--subsample <N>           # compute scores only every Nth frame (default 1 = all frames)
```

`--subsample` trades precision for speed. Pooled scores are still computed over
the sampled subset, so keep it at 1 for final reports.

## Input colorimetry

```text
--color_range_ref/_dist <limited|full|unknown>
--color_primaries_ref/_dist <bt709|bt2020|unknown>
--color_trc_ref/_dist <bt709|smpte2084|pq|unknown>
--color_matrix_ref/_dist <bt709|bt2020nc|ictcp|unknown>
```

A raw `.yuv` or `.y4m` file carries no colorimetry, so each input is described
separately: the `_ref` flags describe the reference, the `_dist` flags the
distorted video. Give all four attributes of an input or none of them; a partly
specified input is a usage error. `pq` is an alias of `smpte2084`.

Only a model that declares a [`conversion_target`](../models/v1.md#model-declared-conversion-target)
reads these flags: libvmaf then converts both inputs to the model's colorspace
(and pixel format and bit depth, where the model pins them) before it extracts
features, and refuses a run whose input colorimetry is missing
(`libvmaf returned -22`, with a message naming the missing attributes). Models
without a `conversion_target`, including every shipped VMAF model, ignore the
flags and score the pictures as they are. The conversion needs a `vmaf` built
with `-Denable_zimg=true` ([build flags](../development/build-flags.md)); without
it a run that needs a conversion fails with `-ENOTSUP`.

```bash
vmaf -r ref.yuv -d dist.yuv -w 576 -h 324 -p 420 -b 10 -m path=hdr_model.json \
  --color_range_ref limited --color_primaries_ref bt2020 --color_trc_ref pq --color_matrix_ref bt2020nc \
  --color_range_dist limited --color_primaries_dist bt2020 --color_trc_dist pq --color_matrix_dist bt2020nc
```

## Input read-ahead

`vmaf` reads the reference and the distorted input on two reader threads, each
up to two frames ahead of scoring
([ADR-1366](../adr/1366-cli-frame-readahead.md)). There is no flag to set. The
measurements are in [Research-1366](../research/1366-cli-frame-readahead.md).

Reading the files and copying each frame into a libvmaf picture then overlaps
feature extraction, and the two inputs are read at the same time. Runs whose
per-frame cost was mostly reading gain the most:

- At 3840x2160 8-bit 4:2:0, `--feature psnr` drops from about 7 to 8 ms to
  about 3.5 to 4 ms per frame on the CPU, with or without `--threads`.
- The `psnr`, `motion` and `adm` twins on an Intel Arc B580
  (`--backend sycl`) drop from about 8 ms to about 4 ms.
- A run already limited by feature extraction, such as the default model on 16
  CPU threads, runs at the same speed as before.

### What stays the same

Read-ahead does not change what is scored:

- The scoring loop receives the same frames, in the same order and pairs, and
  hands them to libvmaf from the same thread as before. Scores are identical,
  including at `--precision max`.
- `--frame_cnt N` stops each reader after `N` frames; nothing past frame `N`
  is read.
- `--frame_skip_ref` and `--frame_skip_dist` skip their frames before the
  readers start.
- The [exit codes](#exit-codes) are unchanged: a stream that ends early still
  exits 0 with the `ended before` warning, and a failed read still exits 102
  without writing a report.

### What changes

| Aspect | Change |
| --- | --- |
| Memory | The picture pool holds four more pictures, two per input: about 50 MB at 3840x2160 8-bit 4:2:0, about 100 MB at 10-bit. The `picture pool: N pictures pre-allocated` line shown on a terminal counts them. |
| Threads | Two more threads exist while frames are being scored. |
| Diagnostics | A reader can reach a damaged frame of its input before the scoring loop stops at the end of the other input. The reader's own message, for example `Error reading YUV frame data.`, is then printed although the run scores the common frames and exits 0. |

### Main-thread fallback

Both inputs are read on the main thread, as before, when:

- on Linux and macOS, both paths name the same file, pipe or device (the same
  device and inode). Two readers of one pipe would each receive whichever
  frames they reached first. `--no-reference` opens the distorted input twice,
  so there it reads on the main thread;
- on Windows, either input is not a regular file (a pipe or a console).
  Windows reports no inode to compare, and two separately opened regular files
  never share a read position;
- a reader thread cannot be created.

## Preset bundles

```text
--aom_ctc v1.0 | v2.0 | v3.0 | v4.0 | v5.0 | v6.0 | v7.0
--nflx_ctc v1.0
```

These expand to a canonical model and feature list for
[AOM](../metrics/ctc/aom.md) and Netflix common-test-conditions reports. For
example, `--aom_ctc v7.0` is equivalent to:

```text
--model version=vmaf_v0.6.1:name=vmaf
--model version=vmaf_v0.6.1neg:name=vmaf_neg
--feature psnr=reduced_hbd_peak=true:enable_apsnr=true:min_sse=0.5
--feature ciede
--feature float_ssim=scale=1:enable_db=true:clip_db=true
--feature float_ms_ssim=enable_db=true:clip_db=true
--feature psnr_hvs
--feature cambi
# plus common_bitdepth=on (forces reference + distorted to the same bitdepth)
```

`--aom_ctc proposed` is deprecated and errors out with an explanation.

<!-- The old anchor stays for ADR-0234, whose Accepted body links to it. -->
<a id="tiny-ai-flags-fork-added"></a>

## Tiny-AI flags

These flags load an ONNX tiny model alongside the classic models. See
[../ai/inference.md](../ai/inference.md) for the full walkthrough and the
per-model registry (`model/tiny/registry.json`, sha256 pins, known
limitations).

| Flag | Default | Effect |
| --- | --- | --- |
| `--tiny-model <path>` | none | Load a `.onnx` tiny model alongside classic models. |
| `--tiny-device <ep>` / `--dnn-ep <ep>` | `auto` | ONNX Runtime execution provider; see below. |
| `--tiny-threads <N>` | `0` (ORT default) | CPU EP intra-op threads. |
| `--tiny-fp16` | off | Request fp16 I/O where the EP supports it. |
| `--tiny-model-verify` | off | Require Sigstore-bundle verification of the loaded model; see [Sigstore verification](#sigstore-bundle-verification). |
| `--no-reference` | off | No-reference (NR) mode; see [NR mode](#no-reference-mode). |
| `--tiny-codec`, `--tiny-preset`, `--tiny-crf` | see below | Codec-context inputs; see [Codec-context flags](#codec-context-flags). |
| `--tiny-resize <mode>` | `disabled` | Auto-resize for NCHW models; see [Resize mode](#resize-mode). |

Underscore aliases exist for every tiny flag (`--tiny_model`, `--tiny_device`,
`--tiny_threads`, `--tiny_fp16`, `--tiny_model_verify`, `--no_reference`,
`--dnn_ep`, `--tiny_codec`, `--tiny_preset`, `--tiny_crf`, `--tiny_resize`), for
symmetry with the underscore flags upstream uses. `--netflix_compat` is the
underscore alias of `--netflix-compat`.

!!! note "`--tiny-model` composes with `--model`"
    Tiny-AI models are additional scores layered on top of the classic
    SVM/XGBoost prediction, not a replacement for it. Use `--no_prediction` to
    get tiny scores alone
    ([ADR-0023](../adr/0023-tinyai-user-surfaces.md)).

### Execution provider

`--tiny-device` and `--dnn-ep` are equivalent: both select the ONNX Runtime
execution provider and write the same internal setting. `--dnn-ep` follows ORT
terminology; `--tiny-device` predates the alias. Accepted values:

```text
auto | cpu | cuda | openvino | openvino-npu | openvino-cpu | openvino-gpu
     | coreml | coreml-ane | coreml-gpu | coreml-cpu | rocm
```

`openvino-npu` pins `device_type=NPU` (Intel AI-PC); `openvino-cpu` and
`openvino-gpu` pin the OpenVINO CPU or GPU plugin with no fallback. See
[docs/ai/inference.md](../ai/inference.md) for the full matrix.

### Quantised sibling models

When `--tiny-model` names `<stem>.onnx`, the loader inspects the companion
sidecar `<stem>.json`. If the sidecar declares `quant_mode != "fp32"` (such as
`"dynamic"`, `"static"` or `"qat"`), the runtime loads the quantised sibling
`<stem>.int8.onnx` when it is present and valid.

When `--tiny-model` names an explicit `.int8.onnx` path directly, the runtime loads
that artifact directly without attempting to append a redundant `.int8` suffix.

If the int8 artifact is missing, fails the op allowlist, or cannot be opened by
the installed ONNX Runtime, the loader falls back to the fp32 baseline
`<stem>.onnx` rather than failing the run (ADR-1032). The fallback is announced
on the `VMAF_LOG_LEVEL_DEBUG` channel, which the CLI does not expose (it runs
at `VMAF_LOG_LEVEL_INFO`), so it is silent on the command line by design. API
callers that set `VmafConfiguration.log_level = VMAF_LOG_LEVEL_DEBUG` see which
graph was opened.

### No-reference mode

`--no-reference` puts the CLI into no-reference (NR) mode (ADR-0520):

1. `--reference` / `-r` is no longer required. The CLI opens the distorted
   source twice (two `video_input` handles backed by the same file), and the
   rank-4 tiny-model dispatch reads picture bytes from the slot that would have
   held the reference, so the model sees the distorted frame.
2. `--tiny-model` becomes **mandatory**, because no classic NR scorer exists in
   the fork. Omitting it prints
   `--no-reference requires --tiny-model; no classic NR scorer exists`.
3. NR mode forces `--no_prediction`. Classic SVM scorers, including the default
   model, consume FR feature columns (`vif_*`, `adm2`, `motion2`) that cannot be
   computed without a reference, so no classic model is scored.
4. The tiny model must accept a rank-4 single-luma input (`[1, 1, H, W]` with
   fully resolved spatial dims matching your distorted source). Rank-2
   feature-vector tiny models (ADR-0518) load but always score `0.0` in NR
   mode, because their input features are derived from the reference.
5. The JSON / XML / CSV report contains only the tiny-AI feature column the
   model wrote; no `pooled_metrics` block exists while `--no_prediction` is
   active.

### Codec-context flags

```text
--tiny-codec <name>            # encoder identity for codec-conditioned tiny models
                                # (libx264, libx265, libsvtav1, libvpx-vp9, h264_nvenc, ...)
--tiny-preset <name>           # encoder preset string (medium, slow, p4, 5, ...)
--tiny-crf <0..63>             # CRF / QP integer; values above 63 clamp at 63
```

These flags drive `vmaf_dnn_set_codec_context()` on the tiny model, and
`--tiny-resize` drives `vmaf_dnn_set_resize_mode()`; see
[api/dnn.md](../api/dnn.md#codec-aware-tiny-model-inputs-vmaf_dnn_set_codec_context).

Codec-conditioned tiny models (for example the v2 ladder regressor) accept a
small categorical block alongside the per-frame features: encoder identity,
preset ordinal and CRF / QP. The CLI sets this block once at model-load time.
Setting any of the three flags to a non-default value enables the path. See
[ADR-0522](../adr/0522-tiny-codec-preset-crf-cli-flags.md) for the categorical
encoding rationale.

| Flag | Default | Notes |
| --- | --- | --- |
| `--tiny-codec` | none (required by codec-aware models) | Must match an entry of the model sidecar's `encoder_vocab`; the vocabulary differs per model (`fr_regressor_v2` lists 11 encoders and `unknown`, `fr_regressor_v3` 16 encoders and no `unknown`). Common ffprobe aliases (`h264`, `hevc`, `av1`, `vp9`, `vvc`) are accepted. |
| `--tiny-preset` | ordinal 5 (medium-equivalent) | Encoder-specific; mirrors `train_fr_regressor_v2.py::PRESET_ORDINAL`. A model trained with one preset value (`fr_regressor_v3`) ignores it and logs a warning. |
| `--tiny-crf` | none (required with `--tiny-codec` / `--tiny-preset`) | Normalised as the model's sidecar declares: clamped to [0, 63] and divided by 63 by default, min-max over 19..37 for `fr_regressor_v3` ([ADR-1558](../adr/1558-codec-block-encoding-from-sidecar.md)). |

!!! warning "Unknown codec names are rejected"
    A `--tiny-codec` value that is not in the model's `encoder_vocab` stops the
    run: `vmaf` prints `--tiny-codec '<name>' not found in model encoder_vocab`
    and exits non-zero. A codec flag on a model that has no codec block fails
    the same way (`--tiny-codec / --tiny-preset / --tiny-crf require a
    codec-aware tiny model`). A codec-aware model run without `--tiny-codec`
    stops on the first frame (`tiny model <name> is codec-aware: ...`), and
    `--tiny-codec` or `--tiny-preset` without `--tiny-crf` stops at load
    ([ADR-1520](../adr/1520-tiny-model-feature-inputs-at-flush.md)). Pass
    `--tiny-codec unknown` when the encoder is not known and the model's
    vocabulary has that entry.

### Resize mode

`--tiny-resize` ([ADR-0550](../adr/0550-tiny-model-auto-resize.md)) is required
when the source frame size (`--width` / `--height`) differs from the tiny
model's declared input shape. The value is checked at parse time: a typo prints
`--tiny-resize must be one of: bilinear, nearest, bicubic, disabled`.

| `--tiny-resize` | Filter | Score-stable? |
| --- | --- | --- |
| `disabled` | None; a size mismatch fails with `-ERANGE` (the default) | Strict |
| `bilinear` | OpenCV `INTER_LINEAR` / torchvision `BILINEAR` | Yes, the convention used by every shipped NR / image-input model |
| `nearest` | OpenCV `INTER_NEAREST` | Yes, deterministic floor of the source coordinate |
| `bicubic` | Separable Catmull-Rom (`a = -0.5`); torchvision `BICUBIC` | Yes, exporter parity |

The three filter modes produce scores that differ by about 2% on the same
input. Treat the filter as a model hyperparameter and pin it alongside the model
checkpoint.

### Sigstore bundle verification

`--tiny-model-verify` is a boolean flag (no argument). Before the model is
loaded into ORT, the loader runs `cosign verify-blob` against the model's
Sigstore bundle. On success the loader proceeds; on failure the process exits
non-zero with a diagnostic on stderr
(`--tiny-model-verify: signature verification failed for <path> (errno <n>)`).

The bundle is not derived from the model file name. The loader reads
`registry.json` from the directory that holds the model (by default
`model/tiny/registry.json`), finds the entry whose `onnx` file name matches, and
uses that entry's `sigstore_bundle` field, for example
`dists_sq.onnx.sigstore.json`. Verification therefore needs:

1. a model registered in the `registry.json` next to it;
2. the bundle file named by that entry;
3. the `cosign` binary on the host's `PATH`;
4. a POSIX host. On Windows verification returns `-ENOSYS` and the run fails.

Use it in production inference pipelines that need supply-chain verification of
model integrity, for example a release runner that pulls a fork-signed `.onnx`
from an artifact store and refuses to score with an unsigned or tampered model.
For local development against an unsigned checkpoint, omit the flag.

Every failure exits non-zero before any inference runs: `cosign` missing from
`PATH`, a missing or invalid bundle, an unregistered model, or a
`cosign verify-blob` rejection (invalid signature, digest mismatch or rejected
certificate identity). See
[ADR-0211](../adr/0211-model-registry-sigstore.md) for the registry schema and
[../ai/inference.md](../ai/inference.md) for the signed-model workflow.

## Logging and misc

| Flag | Short | Effect |
| --- | --- | --- |
| `--help` | | Print the flag reference to stdout and exit 0. |
| `--quiet` | `-q` | Disable the FPS meter when run in a TTY. |
| `--no_prediction` | `-n` | Skip final model prediction; extract features only. Useful for feeding raw features into a custom pool. |
| `--netflix-compat` | | Restore Netflix-upstream legacy defaults: CPU backend, `%.6f` precision and the `vmaf_v0.6.1` default model. Underscore alias `--netflix_compat`. See [vmafx-cli.md](vmafx-cli.md). |
| `--list-backends` | | Print the backends this binary was built with and which of them initialise here, as JSON on stdout, and exit 0; see [Which backends this binary can use](#which-backends-this-binary-can-use). |
| `--version` | `-v` | Print the `libvmaf` version and git SHA to stderr and exit 0. |

CUDA-initialization, luminance, SpEED and VIF diagnostics are complete,
newline-terminated stderr records. The logger supplies the `libvmaf ERROR`
level prefix, so the message bodies do not repeat `Error:`. Log consumers
should match the level token and message body, not the old duplicate
`libvmaf ERROR Error:` spelling from CUDA initialization failures.

## Windows

### UTF-8 paths

On Windows, VMAFx-owned file operations interpret path strings as UTF-8 and
convert them to UTF-16 before calling the wide Windows runtime APIs. This
covers reference and distorted inputs, `--output`, JSON and ONNX model paths,
CAMBI heatmap directories and the paths used by the companion tools. Names with
accented or CJK characters reach the exact requested file rather than an
ANSI-code-page approximation. POSIX path handling is unchanged.

### UTF-8 arguments

The `vmaf` and `vmafx` executables enter through `wmain` on Windows and convert
every UTF-16 argument to strict UTF-8 before the shared CLI parser runs. This
keeps non-ASCII input, output and model paths independent of the active ANSI
code page. An argument containing an invalid UTF-16 sequence fails before
scoring instead of being replaced or misdirected. POSIX argument handling is
unchanged.

### Limits

Internal conversions accept paths shorter than 4096 UTF-8 bytes; longer or
malformed UTF-8 paths fail instead of being truncated. The vendored Pelorus CSV
parser is a documented exception until its upstream source adopts the same
contract. See [ADR-1182](../adr/1182-windows-utf8-path-contract.md) for the
exact migrated surfaces and the original library/CLI scope split.

### Console output

The interactive progress line (frame counter, spinner, FPS) is written to
stderr whenever stderr is a TTY and `--quiet` is not set. The spinner uses
Unicode braille glyphs and an ANSI erase-to-end-of-line sequence, which a
Windows console does not render correctly by default: under the conhost default
code page (cp437) each two-glyph frame decodes as six garbage characters, under
cp936 as replacement boxes, and legacy conhost prints the erase sequence
literally as `<-[K`.

Since ADR-1166 the CLI handles this itself. On Windows it:

1. records the current console output code page and stderr console mode,
2. switches the console to UTF-8 (`CP_UTF8`) and enables
   `ENABLE_VIRTUAL_TERMINAL_PROCESSING`,
3. restores both on exit, including error exits, so your shell is left as it
   was found,
4. and, if the console refuses either change, falls back to a pure-ASCII
   spinner (`|` `/` `-` `\\`) and pads with spaces instead of emitting the
   erase sequence.

There is no flag for this and nothing to configure. `--quiet` still suppresses
the progress line, and redirecting stderr to a file or pipe suppresses it too.
On Linux and macOS the emitted bytes are unchanged. Reported upstream as
[Netflix/vmaf#743](https://github.com/Netflix/vmaf/issues/743).

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success, or `--help` / `--version` / `--list-backends` invocation. |
| 1 | Any parse / I/O / runtime error. `vmaf` writes a diagnostic to stderr before exiting. |
| 100 | Explicit `--backend <name>` requested but the backend is not compiled in or failed to initialise (ADR-0498, ADR-0543). |
| 101 | No frames were decoded (empty or too-short input, or a `--frame_skip_*` value past end-of-stream). `vmaf` writes `no frames decoded ...` to stderr. |
| 102 | An input stream failed to read (truncated file, unreadable media, I/O error). `vmaf` writes `problem while reading pictures` to stderr and writes **no** output file, so a partial score cannot be mistaken for a complete one (ADR-1262). |
| 234 (also other values) | libvmaf failed to score a frame it had read: a feature extractor refused the frame or its options (for example `float_ms_ssim` on a picture below 176x176, or an unsupported viewing geometry). The exit status is the libvmaf error code modulo 256 on every platform, as a non-negative value (`-EINVAL` is 234; Windows does not report the raw 32-bit negative code). `vmaf` writes `problem scoring picture N: libvmaf returned E` to stderr, after the libvmaf message that names the extractor; before 2026-10 it wrote `problem reading pictures`, which read like an input failure. |

Apart from these codes, `libvmaf` does not surface granular error codes at the
process boundary: the specific `VMAF_ERR_*` code from the C API is logged to
stderr but collapsed to exit 1 from the CLI.

### Backend not available

A backend requested with `--backend` that is not compiled in is never replaced
silently. For example, `--backend cuda` on a build without CUDA prints
`vmaf: --backend cuda requested but this libvmaf was built without cuda support;
refusing to silently fall back to CPU (ADR-0498)` and exits 100.

### Shorter streams and write failures

A stream that simply **ends earlier than its partner** is not an error. `vmaf`
writes `"<path>" ended before "<path>".` to stderr, scores the frames the two
have in common and exits 0; scoring a shorter distorted clip against a longer
reference is a supported use. Exit 102 is reserved for a read that *failed*.

A failed output-file write (bad path, full disk, permission denied) exits
non-zero: `vmaf` writes `problem writing output to <path> (err=<n>)` to stderr,
where `<n>` is the negative `VMAF_ERR_*` code, instead of exiting 0 over a stale
or partial file.

### Context cleanup

Context cleanup is part of success. The CLI makes at most two immediate
`vmaf_close()` attempts. If both fail, it prints
`vmaf: context cleanup failed after 2 attempts (err=<n>); retaining dependent resources`
and exits non-zero. Imported backend state and models deliberately stay alive
until process exit instead of being freed beneath the retained context.

The `vmaf_bench` and `vmaf_vpl` tools use the same two-attempt rule and the same
diagnostic shape with their own command prefix. Embedders should follow the
retry ownership contract in [the C API](../api/index.md#core-lifecycle-api).

## Worked example: the upstream golden pair

Download the canonical Netflix test pair from upstream:

```shell
curl -sSLO https://github.com/Netflix/vmaf_resource/raw/master/python/test/resource/yuv/src01_hrc00_576x324.yuv
curl -sSLO https://github.com/Netflix/vmaf_resource/raw/master/python/test/resource/yuv/src01_hrc01_576x324.yuv
```

Run VMAF (pinned to `vmaf_v0.6.1`) plus PSNR:

```shell
./build/tools/vmaf \
  --reference  src01_hrc00_576x324.yuv \
  --distorted  src01_hrc01_576x324.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --model version=vmaf_v0.6.1 \
  --feature psnr \
  --output scores.xml
```

On a terminal, stderr shows the banner, the progress meter and the pooled
score (nothing is printed when stderr is redirected):

```text
VMAF version <version>
48 frames  <fps> FPS
vmaf_v0.6.1: 76.667831
```

The head of `scores.xml` (the root `version` attribute is the `vmaf --version`
string, for example `v1.0.0-rc.2-310-g1b1663830`; the metric names and values
shown are those of the Netflix pair):

```xml
<VMAF version="<version>">
  <params qualityWidth="576" qualityHeight="324" />
  <fyi fps="..." />
  <frames>
    <frame frameNum="0" integer_adm2="0.962084" ... psnr_y="34.760779" ... vmaf="83.856284" />
    ...
  </frames>
  <pooled_metrics>
    <metric name="vmaf" min="71.174759" max="87.180962" mean="76.667831" harmonic_mean="76.508907" />
    ...
  </pooled_metrics>
</VMAF>
```

With `--precision max` the pooled mean prints as `76.667831491355784`. Without
`--model version=vmaf_v0.6.1`, the default model gives a pooled mean of
`82.816060` for the same pair.

The `vmaf_v0.6.1` pooled mean on this pair is one of the three Netflix CPU
goldens preserved as a required CI gate. The CLI path is pinned to
`76.66783025` in
[`python/test/vmafexec_test.py`](../../python/test/vmafexec_test.py); see
[ADR-0024](../adr/0024-netflix-golden-preserved.md).

## Flag interactions and pitfalls

- **`.yuv` without geometry.** `--reference foo.yuv` without
  `--width/--height/--pixel_format/--bitdepth` errors out. `.y4m` carries
  geometry in its header; `.yuv` does not.
- **Duplicate model names.** Each `--model` needs a unique `name=`. If the
  same built-in version is loaded twice, set `name=` on at least one.
- **`--no_prediction` with `--model`.** `--no_prediction` skips model
  prediction but not loading: the model still selects which features to
  extract. To extract only the `--feature` list, omit `--model` and pass
  `--no_prediction`.
- **Default `%.6f` rounding.** The default (and `--precision legacy`) hides
  differences of about 1e-6 that `--precision=max` shows. Use `max` to compare
  scores numerically; the default exists for byte-for-byte agreement with
  pre-fork Netflix output, which the CPU golden gate depends on.
- **`--tiny-model` vs `--model`.** They compose; see the note under
  [Tiny-AI flags](#tiny-ai-flags).
- **`--no_cuda` + `--no_sycl` together.** Forces CPU-only even on a build with
  both GPU backends compiled in. Useful for cross-backend diff sessions.

## Related

- [bench.md](bench.md) — `vmaf_bench` micro-benchmark harness.
- [vmaf-perShot.md](vmaf-perShot.md) — per-shot CRF predictor sidecar
  ([ADR-0222](../adr/0222-vmaf-per-shot-tool.md)).
- [ffmpeg.md](ffmpeg.md) — using the VMAF filter inside `ffmpeg`.
- [python.md](python.md) — Python bindings for the CLI.
- [precision.md](precision.md) — dedicated `--precision` flag walkthrough.
- [../backends/index.md](../backends/index.md) — runtime backend dispatch rules.
- [../metrics/features.md](../metrics/features.md) — per-feature identifiers
  and options.
- [../ai/inference.md](../ai/inference.md) — tiny-AI inference walkthrough.
- [ADR-0119](../adr/0119-cli-precision-default-revert.md) (current precision
  default; supersedes [ADR-0006](../adr/0006-cli-precision-17g-default.md)),
  [ADR-0023](../adr/0023-tinyai-user-surfaces.md),
  [ADR-0024](../adr/0024-netflix-golden-preserved.md),
  [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md).

## Former section names

Older pages and records link to these headings; each points to the section
that now holds its content.

### Tiny-AI flags (fork-added)

Now under [Tiny-AI flags](#tiny-ai-flags).

### Codec-context flags (fork-added)

Now under [Codec-context flags](#codec-context-flags).

## History

Behaviour that older builds had, kept so old logs and scripts can be
interpreted.

- **`--threads` with a GPU backend
  ([ADR-1197](../adr/1197-gpu-threaded-flush-ownership.md)).** Builds before
  this change aborted with `libvmaf ERROR context could not be synchronized`
  and exit 234 whenever `--threads` was combined with `--backend cuda` or
  `--backend sycl`, for every thread count including `--threads 1`. The message
  was misleading: the GPU context was healthy and the failure came from the
  feature-extractor flush. If you see it, you are on an older build.
  `testdata/bench_all.sh` pins `--threads 1`, so GPU rows produced by older
  builds of that harness were failures rather than measurements.
- **Option-string escapes
  ([ADR-1190](../adr/1190-cli-option-string-escape-grammar.md),
  [ADR-1355](../adr/1355-cli-option-value-backslashes.md)).** Before ADR-1190
  there was no escape mechanism: `path=C:\models\m.json` was rejected with
  `bad option string "\models\m.json"`, and `path=/a/dir=eq/m.json` was
  silently truncated to `/a/dir`. Until ADR-1355 the key escapes also applied
  to values, so `..\..\models\m.json` was read as `....\models\m.json`,
  `\\server\share` as `\server\share`, and `C:\models\.cache` as
  `C:\models.cache`.
- **Default model
  ([ADR-1169](../adr/1169-default-model-v1-0-16.md)).** The default was
  `vmaf_v0.6.1` before this fork moved to `vmaf_v1.0.16_3d0h`.
- **Proposed: `--gpu-calibrated`
  ([ADR-0234](../adr/0234-gpu-gen-ulp-calibration.md)).** A future flag would
  opt into a per-architecture ULP calibration head that maps raw GPU scores to
  their CPU-equivalent values, closing the cross-backend divergence of about
  1e-4 that currently sits within `places=4` tolerance. It is not shipped: the
  flag does not exist and the calibration model is not trained. The ADR lists
  the measurement gates that must clear first.
