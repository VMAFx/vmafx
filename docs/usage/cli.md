<!-- markdownlint-disable MD060 -->
# `vmaf` — command-line reference

`vmaf` is the main CLI binary shipped with this fork. It takes a reference /
distorted video pair, runs one or more VMAF models (plus any additional feature
extractors), and writes per-frame + pooled scores to an XML / JSON / CSV /
subtitle log.

> **Scope.** This page is the canonical flag reference for the `vmaf` binary
> in the VMAFx fork. It supersedes the abbreviated help string in
> [`core/tools/README.md`](../../core/tools/README.md) — the code's
> `--help` is authoritative for the *set* of flags at any given commit; this
> page adds defaults, interactions, and runnable examples per
> [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md).
>
> For the `vmaf_bench` micro-benchmark binary, see [bench.md](bench.md).
> For FFmpeg integration (the `libvmaf` filter), see [ffmpeg.md](ffmpeg.md).
> For the Python bindings, see [python.md](python.md).

## Quick start

```shell
# .y4m pair — no geometry flags needed
vmaf --reference ref.y4m --distorted dist.y4m

# .yuv pair — geometry is mandatory
vmaf \
  --reference ref.yuv \
  --distorted dist.yuv \
  --width 1920 --height 1080 \
  --pixel_format 420 --bitdepth 8 \
  --output scores.xml
```

Default behaviour when no `--model` is passed: the built-in
`vmaf_v1.0.16_3d0h` model is loaded automatically. Default output format when
no `--xml|--json|--csv|--sub` is passed: XML.

> **The default changed in this fork.** It was `vmaf_v0.6.1`, and upstream
> Netflix still defaults to that, so the same command produces different numbers
> on upstream and here. Pin `--model version=vmaf_v0.6.1` to get the old values.
> On the standard 576x324 test pair the pooled VMAF moves from `76.667831` to
> `82.816059`. See [ADR-1169](../adr/1169-default-model-v1-0-16.md) for why, and
> [models/v1.md](../models/v1.md) for the v1 model family.

## Required input flags

| Flag | Short | Argument | Required | Notes |
| --- | --- | --- | --- | --- |
| `--reference` | `-r` | path | **yes** | `.y4m` or `.yuv` path. |
| `--distorted` | `-d` | path | **yes** | `.y4m` or `.yuv` path. |
| `--width` | `-w` | unsigned | **yes for `.yuv`** | Ignored for `.y4m` (embedded). |
| `--height` | `-h` | unsigned | **yes for `.yuv`** | Ignored for `.y4m`. |
| `--pixel_format` | `-p` | `420` \| `422` \| `444` | **yes for `.yuv`** | 420 covers the overwhelming majority of streamable content. |
| `--bitdepth` | `-b` | `8` \| `10` \| `12` \| `16` | **yes for `.yuv`** | 10 and 12 bit require a 10-/12-bit aware model (e.g. `vmaf_b_v0.6.3` for banding sensitivity). |

If any of `--width`, `--height`, `--pixel_format`, `--bitdepth` is supplied
the input is treated as raw YUV and **all four** become mandatory.

## Models

The `--model / -m` flag takes a colon-delimited key/value string:

```text
--model path=<file>         # load a .json model from disk
--model version=<builtin>   # load a built-in model by name
--model path=...:name=<str> # rename the metric in the output log
--model version=...:disable_clip          # disable score clipping to [0, 100]
--model version=...:enable_transform      # apply transform
```

Built-in model versions (compiled into `libvmaf` via `-Dbuilt_in_models=true`,
default `true`):

| Version | Purpose |
| --- | --- |
| `vmaf_v1.0.16_3d0h` | **Default.** v1.0.16 standard 1080p model, 3H viewing distance. |
| `vmaf_v1.0.16_1d5h_2160` | v1.0.16 4K model, 2160p at 1.5H. Used by the fork's 4K resolution ladder. |
| `vmaf_v1.0.16_5d0h` | v1.0.16 phone model (1080p at 5H). |
| `vmaf_v1.0.16_3d0h_2160` | v1.0.16 consumer 4K (2160p at 3H); operates on a [0, 110] range. |
| `vmaf_v0.6.1` | Previous default, and still upstream's. 1080p training set, classic release. |
| `vmaf_v0.6.1neg` | Negative-gain (NEG) — non-enhancing; recommended for encoder A/B where one encoder may artificially sharpen. There is no NEG counterpart to any v1.0.16 model, so asking for NEG also selects the v0.6.1 generation. |
| `vmaf_b_v0.6.3` | Banding-aware variant (used with CAMBI). |
| `vmaf_4k_v0.6.1` | 4K training set. |
| `vmaf_4k_v0.6.1neg` | 4K + NEG. |

Float-precision variants (`vmaf_float_v0.6.1`, `vmaf_float_v0.6.1neg`,
`vmaf_float_b_v0.6.3`, `vmaf_float_4k_v0.6.1`) are also resolvable but are
legacy — prefer the integer versions for performance, the float versions
only for bit-exact comparison with older reports.

`--model` can be passed multiple times to run several models in one pass; each
model must have a unique `name=` (or the CLI errors out). Example running VMAF +
VMAF-NEG side-by-side:

```shell
vmaf -r ref.y4m -d dist.y4m \
  --model version=vmaf_v0.6.1:name=vmaf \
  --model version=vmaf_v0.6.1neg:name=vmaf_neg \
  --output scores.json --json
```

## Additional features

The `--feature` flag enables extra metrics (beyond whatever the model already
consumes). Syntax is the same colon-delimited key/value form as `--model`:

```shell
--feature psnr
--feature psnr=enable_chroma=true:enable_apsnr=true
--feature float_ssim=enable_db=true:clip_db=true
--feature cambi
--feature ciede
--feature psnr_hvs
--feature brisque
--feature brisque=model_path=/path/to/brisque_live.model
```

The `brisque` no-reference metric ships its trained model embedded in the
binary, so it needs no extra arguments; `model_path` overrides it with an
on-disk libsvm model (and is required only for builds with `built_in_models`
disabled). See [../metrics/brisque.md](../metrics/brisque.md).

See [../metrics/features.md](../metrics/features.md) for the full list of feature
identifiers and per-feature options.

Option validation is strict: specifying an unknown option key or a typo
(e.g., `--feature adm=adm_csf_moed=2`) causes libvmaf to reject the configuration
immediately with `libvmaf ERROR feature extractor '<name>': unknown option '<key>'`
and exit non-zero ([ADR-1183](../adr/1183-model-options-gate-gpu-twin-selection.md)).

## Output

| Flag | Default | Notes |
| --- | --- | --- |
| `--output` / `-o <path>` | stdout line + no file | Writes the per-frame + pooled log to `<path>`. |
| `--xml` | **default** | XML report (upstream-compatible). |
| `--json` | | JSON report. |
| `--csv` | | One row per frame. |
| `--sub` | | SubRip subtitle format — useful for overlaying scores during playback. |

Stderr always carries a short progress line plus the final pooled-mean VMAF score,
regardless of `--output`.

### Score precision (fork-added)

```text
--precision N          # printf "%.<N>g", N in 1..17
--precision max|full   # printf "%.17g" — IEEE-754 round-trip lossless (opt-in)
--precision legacy     # printf "%.6f"  — synonym for the default
```

The fork's default is `%.6f` (see
[ADR-0119](../adr/0119-cli-precision-default-revert.md), which supersedes
[ADR-0006](../adr/0006-cli-precision-17g-default.md)), matching upstream
Netflix output byte-for-byte so the CPU golden gate passes without explicit
flags. Pass `--precision=max` whenever you need IEEE-754 round-trip lossless
output (cross-backend numeric diff, archival reports, any consumer that
re-parses scores into doubles and compares them). Affects XML, JSON, CSV,
SUB, and stderr consistently.

See [precision.md](precision.md) for the full table of when to pick each mode.

## Backend selection

Build-time: each backend is opt-in via a meson flag. At **runtime**, backend
selection is per-invocation through flags on `vmaf` — there is **no** environment
variable that overrides it.

| Flag | Default | Effect |
| --- | --- | --- |
| `--no_cuda` | off | Forbid CUDA dispatch even if the CUDA backend is built in. |
| `--no_sycl` | off | Forbid SYCL dispatch even if the SYCL backend is built in. |
| `--sycl_device <N>` | auto (first GPU) | Pick SYCL device by ordinal from the oneAPI device list. |
| `--no_hip` | off | Forbid HIP/ROCm dispatch even if the HIP backend is built in. |
| `--hip_device <N>` | disabled (opt-in) | Pick HIP/ROCm device by ordinal. Pass `0` for the first AMD GPU. Without this flag the HIP backend is never used, even when the binary was built with `-Denable_hip=true`. See [../backends/hip/overview.md](../backends/hip/overview.md). |
| `--no_metal` | off | Forbid Metal dispatch even if the Metal backend is built in (macOS only). |
| `--metal_device <N>` | disabled (opt-in) | Pick Metal GPU by ordinal (macOS only). Pass `0` for the first Metal device (typically the integrated Apple GPU on Apple Silicon). Without this flag the Metal backend is never used, even on macOS builds. See [../backends/metal/index.md](../backends/metal/index.md). |
| `--backend <name>` | `auto` | Exclusive backend selector — `auto` (default; whichever backends are built compete by registry order), `cpu`, `cuda`, `sycl`, `hip`, `metal`. Setting a specific backend disables the others via the matching `--no_X` flags BEFORE dispatch and pins the device index for the chosen backend (`gpumask=0` for CUDA, `sycl_device=0` for SYCL, `hip_device=0` for HIP, `metal_device=0` for Metal). (The `vulkan` token and `--no_vulkan` / `--vulkan_device` flags were removed in ADR-0726.) |
| `--cpumask <bitmask>` (`-c`) | all ISAs enabled | Mask out specific CPU ISAs (e.g. force scalar, disable AVX-512). Values are fork-internal — see `core/src/cpu.h`. |
| `--gpumask <bitmask>` | all GPU ops enabled | Mask out specific GPU ops. |
| `--threads <N>` | host `nproc` | CPU-side worker thread count. |

> **Vulkan backend removed (ADR-0726):** The `--no_vulkan`, `--vulkan_device`,
> and `--backend vulkan` flags no longer exist. Passing them produces an
> unrecognised-option error. See
> [../backends/vulkan/overview.md](../backends/vulkan/overview.md) for
> historical context.

If neither backend is built in, these flags are silently inert. See
[../backends/index.md](../backends/index.md) for the runtime-dispatch rules and
which features have GPU/SIMD twins.

> **Future work — `--gpu-calibrated`** (proposed,
> [ADR-0234](../adr/0234-gpu-gen-ulp-calibration.md)). A future flag will
> opt into a per-arch ULP calibration head that maps raw GPU scores to their
> CPU-equivalent values, closing the ~1e-4 cross-backend divergence
> currently within `places=4` tolerance. **Not shipped in this release** —
> the flag does not exist yet; the calibration model is not trained yet.
> See the ADR for measurement gates that have to clear first.

## Frame range

```text
--frame_cnt <N>           # stop after N frames (both streams)
--frame_skip_ref <N>      # skip the first N frames of the reference
--frame_skip_dist <N>     # skip the first N frames of the distorted
--subsample <N>           # compute scores only every Nth frame (default 1 = all frames)
```

`--subsample` trades precision for speed — pooled scores are still computed
over the sampled subset, so keep it at 1 for final reports.

## Preset bundles

```text
--aom_ctc v1.0 | v2.0 | v3.0 | v4.0 | v5.0 | v6.0 | v7.0
--nflx_ctc v1.0
```

These expand to a canonical model + feature list for
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

`--aom_ctc proposed` is deprecated (errors out with an explanation).

## Tiny-AI flags (fork-added)

```text
--tiny-model <path>            # load a .onnx tiny model alongside classic models
--tiny-device auto|cpu|cuda|openvino|coreml|coreml-ane|coreml-gpu|coreml-cpu|openvino-npu|openvino-cpu|openvino-gpu|rocm
                                # ORT execution provider (default: auto)

--dnn-ep auto|cpu|cuda|openvino|coreml|coreml-ane|coreml-gpu|coreml-cpu|rocm
                                # alias for --tiny-device; uses ORT terminology

                                # openvino-npu pins device_type=NPU (Intel AI-PC);
                                # openvino-cpu / openvino-gpu pin the OpenVINO
                                # CPU / GPU plugin with no fallback. See
                                # docs/ai/inference.md for the full matrix.

--tiny-threads <N>             # CPU EP intra-op threads (0 = ORT default)
--tiny-fp16                    # request fp16 I/O where the EP supports it
--no-reference                 # NR mode; requires a no-reference tiny model
```

`--dnn-ep` and `--tiny-device` are equivalent — they select the ONNX Runtime
execution provider and write to the same internal setting. Use whichever name
is more natural for your script; `--dnn-ep` follows the ORT "execution
provider" terminology, while `--tiny-device` predates the alias.

Underscore aliases (`--tiny_model`, `--tiny_device`, `--tiny_threads`, `--tiny_fp16`,
`--no_reference`, `--dnn_ep`) are accepted for scripting symmetry with the underscore
flags upstream uses.

`--no-reference` puts the CLI into no-reference (NR) mode (ADR-0520):

- `--reference` / `-r` is no longer required. The CLI opens the
  distorted source twice (two `video_input` handles backed by the same
  file) and the rank-4 tiny-model dispatch reads picture bytes from the
  slot that would have held the reference, so the model sees the
  distorted frame.
- `--tiny-model` is now **mandatory** — no classic NR scorer exists in
  the fork. Omitting it returns the diagnostic
  `--no-reference requires --tiny-model; no classic NR scorer exists`.
- The built-in `vmaf_v0.6.1` SVM is auto-suppressed (NR mode forces
  `--no_prediction`); all classic SVM scorers consume FR feature
  columns (`vif_*`, `adm2`, `motion2`) that cannot be computed without
  a reference. To pin a tiny model on top of the SVM you need a
  reference.
- The tiny model must accept a rank-4 single-luma input
  (`[1, 1, H, W]` with fully-resolved spatial dims matching your
  distorted source). Rank-2 feature-vector tiny models (ADR-0518) load
  but always score `0.0` in NR mode, because their input features are
  derived from the reference.
- The JSON / XML / CSV report contains only the tiny-AI feature column
  the model wrote through; no `pooled_metrics` block exists when
  `--no_prediction` is active.

See [../ai/inference.md](../ai/inference.md) for the full tiny-AI CLI
walkthrough and the per-model registry (`model/tiny/registry.json`,
sha256 pins, known limitations).

### Codec-context flags (fork-added)

```text
--tiny-codec <name>            # encoder identity for codec-conditioned tiny models
                                # (libx264, libx265, libsvtav1, libvpx-vp9, h264_nvenc, ...)
--tiny-preset <name>           # encoder preset string (medium, slow, p4, 5, ...)
--tiny-crf <0..63>             # CRF / QP integer; values above 63 clamp at 63
--tiny-resize <mode>           # bilinear | nearest | bicubic | disabled
```

These flags drive `vmaf_dnn_set_codec_context()` and
`vmaf_dnn_set_resize_mode()` on the tiny model — see
[api/dnn.md](../api/dnn.md#codec-aware-tiny-model-inputs-vmaf_dnn_set_codec_context).

**Codec block.** Codec-conditioned tiny models (e.g. the v2 ladder
regressor) accept a small categorical block alongside the per-frame
features: encoder identity, preset ordinal, and CRF / QP. The CLI sets
this block once at model-load time. Unknown encoder names fall back to
the `"unknown"` bucket with a stderr diagnostic; missing `--tiny-codec`
on a model that requires codec context is permitted and routes through
the `"unknown"` bucket silently. Set `--tiny-codec`, `--tiny-preset`,
or `--tiny-crf` to **any** non-default value to enable the path. See
[ADR-0522](../adr/0522-tiny-codec-preset-crf-cli-flags.md) for the categorical
encoding rationale.

**Resize mode** ([ADR-0550](../adr/0550-tiny-dnn-resize-mode.md)).
Required when the source frame size (`--width` / `--height`) differs from
the tiny model's declared input shape:

| `--tiny-resize` | Filter                                                      | Score-stable? |
|-----------------|-------------------------------------------------------------|---------------|
| `disabled`      | None — size mismatch fails with `-ERANGE` (the default)     | Strict        |
| `bilinear`      | OpenCV `INTER_LINEAR` / torchvision `BILINEAR`              | Yes — convention used by every shipped NR / image-input model |
| `nearest`       | OpenCV `INTER_NEAREST`                                      | Yes — deterministic floor of source coord |
| `bicubic`       | Separable Catmull-Rom (`a = -0.5`); torchvision `BICUBIC`   | Yes — exporter parity         |

The three filter modes produce scores that differ by approximately
2% on the same input — treat filter choice as a model hyperparameter
and pin it alongside the model checkpoint. A typo in the resize
keyword surfaces at parse time
(`--tiny-resize must be one of: bilinear, nearest, bicubic, disabled`)
rather than after model load. Underscore aliases (`--tiny_codec`,
`--tiny_preset`, `--tiny_crf`, `--tiny_resize`) are accepted for
scripting symmetry.

### Sigstore bundle verification (fork-added)

```text
--tiny-model-verify            # enable Sigstore bundle verification for the loaded tiny-AI model
```

`--tiny-model-verify` is a **boolean flag** (no argument). It enables
`cosign verify-blob` verification of the Sigstore bundle attached to
the tiny-AI ONNX model **before** the model is loaded into ORT. Both
the model path and its bundle path are inferred from `--tiny-model`:
the bundle is expected at `<model-path>.sigstore` alongside the model
file. Verification is performed in-process by shelling out to the
`cosign` binary on the host's `PATH`; on success the loader proceeds
normally, on failure the process exits non-zero with a diagnostic to
stderr.

When to use it: production inference pipelines that need supply-chain
verification of model integrity — e.g. a release runner that pulls a
fork-signed `.onnx` from an artifact store and refuses to score with
an unsigned or tampered model. For local development against an
unsigned checkpoint, omit the flag.

Failure modes (all exit non-zero before any inference runs):

- `cosign` binary not on `PATH`.
- Bundle path missing, unreadable, or not a valid Sigstore bundle.
- `cosign verify-blob` reports an invalid signature, mismatched
  digest, or rejected certificate identity.

See [ADR-0211](../adr/0211-model-registry-sigstore.md)
for the model-registry schema and the Sigstore-bundle integration
that this flag consumes, and
[../ai/inference.md](../ai/inference.md) for the end-to-end signed-model
workflow.

## Logging and misc

| Flag | Short | Effect |
| --- | --- | --- |
| `--help` | | Print the flag reference to stdout and exit 0. |
| `--quiet` | `-q` | Disable the FPS meter when run in a TTY. |
| `--no_prediction` | `-n` | Skip final model prediction; extract features only. Useful for feeding raw features into a custom pool. |
| `--version` | `-v` | Print `libvmaf` version + git SHA and exit. |

## Windows console output

The interactive progress line (frame counter, spinner, FPS) is written to
stderr whenever stderr is a TTY and `--quiet` is not set. The spinner uses
Unicode braille glyphs and an ANSI erase-to-end-of-line sequence, neither of
which a Windows console renders correctly by default: under the conhost default
code page (cp437) each two-glyph frame decodes as six garbage characters, under
cp936 it decodes as replacement boxes, and legacy conhost prints the erase
sequence literally as `<-[K`.

Since ADR-1166 the CLI handles this itself. On Windows it:

1. records the current console output code page and stderr console mode,
2. switches the console to UTF-8 (`CP_UTF8`) and enables
   `ENABLE_VIRTUAL_TERMINAL_PROCESSING`,
3. restores both on exit — including error exits — so your shell is left as it
   was found,
4. and, if the console refuses either change, falls back to a pure-ASCII
   spinner (`|` `/` `-` `\\`) and pads with spaces instead of emitting the
   erase sequence.

There is no flag for this and nothing to configure; `--quiet` still suppresses
the progress line entirely, and redirecting stderr to a file or pipe suppresses
it as well (the CLI only draws it for a TTY). On Linux and macOS the emitted
bytes are unchanged from previous releases.

Reported upstream as [Netflix/vmaf#743](https://github.com/Netflix/vmaf/issues/743).

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success, or `--help` / `--version` invocation. |
| 1 | Any parse / I/O / runtime error. `vmaf` writes a diagnostic to stderr before exiting. |
| 100 | Explicit `--backend <name>` requested but the backend failed to initialise (ADR-0543). |
| 101 | No frames were decoded (empty or too-short input, or a `--frame_skip_*` value past end-of-stream). `vmaf` writes `no frames decoded ...` to stderr. |

A failed output-file write (bad path, full disk, permission denied) also exits
non-zero: `vmaf` writes `problem writing output to <path> (err=<n>)` to stderr,
where `<n>` is the negative `VMAF_ERR_*` code, instead of exiting 0 over a
stale or partial file.

Apart from the dedicated codes above, `libvmaf` does not surface granular error
codes at the process boundary; the specific `VMAF_ERR_*` code from the C API is
logged to stderr but collapsed to exit 1 from the CLI.

## Worked example — reproducing the upstream golden pair

Download the canonical Netflix test pair from upstream:

```shell
curl -sSLO https://github.com/Netflix/vmaf_resource/raw/master/python/test/resource/yuv/src01_hrc00_576x324.yuv
curl -sSLO https://github.com/Netflix/vmaf_resource/raw/master/python/test/resource/yuv/src01_hrc01_576x324.yuv
```

Run VMAF plus PSNR:

```shell
./build/core/tools/vmaf \
  --reference  src01_hrc00_576x324.yuv \
  --distorted  src01_hrc01_576x324.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --model version=vmaf_v0.6.1 \
  --feature psnr \
  --output scores.xml
```

Expected stderr (CPU path):

```text
VMAF version 3.2.1
48 frames  44.72 FPS
vmaf_v0.6.1: 76.6689050197
```

Expected `scores.xml` head:

```xml
<VMAF version="3.2.1">
  <params qualityWidth="576" qualityHeight="324" />
  <fyi fps="41.98" />
  <frames>
    <frame frameNum="0" integer_adm2="0.96208412..." ... psnr_y="34.76077932..." vmaf="83.8562851..." />
    ...
  </frames>
  <pooled_metrics>
    <metric name="vmaf" min="71.17655..." max="87.18142..." mean="76.66890501..." harmonic_mean="76.51000634..." />
    ...
  </pooled_metrics>
</VMAF>
```

Pooled-mean VMAF for this pair is **76.668905…**. This is one of the three Netflix
CPU goldens preserved verbatim as a required CI gate — see
[ADR-0024](../adr/0024-netflix-golden-preserved.md) and
[`python/test/quality_runner_test.py`](../../python/test/quality_runner_test.py).

## Flag interactions and pitfalls

- **`.yuv` without geometry**. Passing `--reference foo.yuv` without
  `--width/--height/--pixel_format/--bitdepth` errors out. `.y4m` carries geometry
  in the header; `.yuv` does not.
- **Duplicate model names**. Each `--model` must have a unique `name=`. If the same
  built-in version is loaded twice, set `name=` explicitly on at least one.
- **`--no_prediction` with `--model`**. `--no_prediction` skips model prediction
  but does not skip loading — the model is still used to select which features to
  extract. Omit `--model` entirely plus pass `--no_prediction` to extract only the
  features listed via `--feature`.
- **Default `%.6f` truncation**. The default (and `--precision legacy`)
  truncates differences ≤ 1e-6 that would be distinguishable under
  `--precision=max`. Use `max` whenever you need to compare scores
  numerically (cross-backend diff, archival reports). The default mode
  exists for byte-for-byte agreement with pre-fork Netflix output, which
  the CPU golden gate depends on.
- **`--tiny-model` vs `--model`**. These compose — tiny-AI models are
  **additional** scores layered on top of the classic SVM/XGBoost prediction, not
  a replacement for it. Use `--no_prediction` if you want tiny scores alone. See
  [ADR-0023](../adr/0023-tinyai-user-surfaces.md).
- **`--no_cuda` + `--no_sycl` together**. Forces CPU-only even on a build with both
  GPU backends compiled in. Useful for cross-backend diff sessions.

## Related

- [bench.md](bench.md) — `vmaf_bench` micro-benchmark harness.
- [vmaf-perShot.md](vmaf-perShot.md) — per-shot CRF predictor sidecar
  (T6-3b / [ADR-0222](../adr/0222-vmaf-per-shot-tool.md)).
- [ffmpeg.md](ffmpeg.md) — using the VMAF filter inside `ffmpeg`.
- [python.md](python.md) — Python bindings for the CLI.
- [precision.md](precision.md) — dedicated `--precision` flag walkthrough.
- [../backends/index.md](../backends/index.md) — runtime backend dispatch rules.
- [../metrics/features.md](../metrics/features.md) — per-feature identifiers
  and options.
- [../ai/inference.md](../ai/inference.md) — tiny-AI inference walkthrough.
- [ADR-0119](../adr/0119-cli-precision-default-revert.md) (current
  precision default; supersedes
  [ADR-0006](../adr/0006-cli-precision-17g-default.md)),
  [ADR-0023](../adr/0023-tinyai-user-surfaces.md),
  [ADR-0024](../adr/0024-netflix-golden-preserved.md),
  [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md).
x
