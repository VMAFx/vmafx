<!-- markdownlint-disable MD013 MD060 -->
# Scoring API contract

The scoring server (`vmafx-server`) answers over gRPC (`vmafx.v1.VmafxScoring`)
and HTTP (`POST /v1/score`). This page is the contract an integrator builds on:
what a request may carry, what every response reports, what the server
promises about the scores, and how the API may change. The services are
described on [gRPC service](grpc.md) and [REST API](rest.md); the server's
settings on [Server configuration](configuration.md).

## The same request gives the same score everywhere

A request scores the same pair the same way through every VMAFx surface:

| Surface | How the request is spelled |
| --- | --- |
| `vmaf` command line | flags, for example `-w 576 -h 324 -p 420 -b 8 --threads 4` |
| C API | `vmafx_context_create()` with `VmafxContextConfig`, then frames and `vmafx_score_pooled()` |
| gRPC `Score` | `ScoreRequest` with `ScoreOptions` |
| `POST /v1/score` | the same `ScoreRequest` in JSON, with the proto field names |

The server returns lossless scores (`precision` `max`, `%.17g`) unless the
request asks for another precision, so the score it returns is the double
the CLI and the C API compute, bit for bit. The contract test checks it on
the Netflix 576x324 pair for the library default model, `vmaf_v0.6.1` with
four threads and `vmaf_v0.6.1` on every third frame:

```bash
meson test -C build test_vmafx_score_contract   # suite: contract
```

It runs each request through the `vmaf` binary of the build, the C API
(`build/test/vmafx_score_contract`), gRPC `Score` and `POST /v1/score`, and
fails unless all four report the same score and the same library version and
ABI. It skips, with the reason, when Go or the fixtures are missing; a
skipped run is never reported as a pass.

## Requests

```json
{
  "reference": "/data/src01_hrc00_576x324.yuv",
  "distorted": "/data/src01_hrc01_576x324.yuv",
  "model": "vmaf_v0.6.1",
  "options": {"width": 576, "height": 324, "pixel_format": "420", "bitdepth": 8, "threads": 4}
}
```

- `reference`, `distorted`: paths the server can read; `.y4m`, or raw `.yuv`
  with `width`, `height`, `pixel_format` and `bitdepth` in `options`.
- `model`: a model name from the server's model directory. Omitted, the
  library default model (`VMAF_DEFAULT_MODEL_VERSION`, `vmaf_v1.0.16_3d0h`
  in this release).
- `options`: any field of the table below. An unset field keeps the default in
  its row. A value outside the field's range or list, a field the contract
  does not know, or a `device` without a backend that selects devices by
  index is refused (`INVALID_ARGUMENT`, HTTP 400), never ignored. Fields
  marked reserved accept only their default until the release that implements
  them.

## Responses and provenance

The response to the request above (a development build; `version` names the
build that ran):

```json
{
  "score": 76.66783149135578,
  "features": {"vmaf": 76.66783149135578, "integer_adm2": 0.9345057762923995, "integer_motion2": 3.894360826611791},
  "provenance": {
    "library": {"abi_major": 0, "abi_minor": 1, "abi_patch": 5, "active_backend": "cpu",
                "n_extractors": 3, "version": "v1.0.0-rc.2-529-gb8b3af281",
                "build_id": "sha256:...", "models": [{"name": "vmaf", "version": "vmaf_v0.6.1",
                "sha256": "5950d61f...", "flags": "0", "overrides": ""}],
                "features": [...], "annotations": [...], "digest": "sha256:...", ...},
    "model": "vmaf_v0.6.1",
    "model_sha256": "5950d61fa1f861bd45d8149d80539ed9f3376cfc2495b8f0fa8e9f57cb131ee3",
    "backend_used": "cpu",
    "feature_backends": [{"extractor": "adm", "backend": "cpu"}, {"extractor": "motion", "backend": "cpu"},
                         {"extractor": "vif", "backend": "cpu"}],
    "precision": "max"
  }
}
```

Every scoring response carries `provenance`: the gRPC `ScoreResponse`, the
`POST /v1/score` body and the final `AggregateScore` of `ScoreStream`.

| Field | Meaning |
| --- | --- |
| `library` | The library's provenance record (`Provenance`, from `VmafxProvenance`): library, ABI and build, device, options, frames, the models with their SHA-256, the producer of every feature, the command line, and the digests ([Score provenance](../usage/provenance.md)) |
| `model` | The model the server resolved the request's `model` to |
| `model_sha256` | SHA-256 of the model file the server loaded |
| `backend_used` | The backend the extractors ran on (`cpu` when all ran on the CPU) |
| `feature_backends` | The backend of every registered extractor |
| `precision` | The precision the scores were written with; `max` is lossless |

A server whose `vmaf` binary does not report the provenance record (a binary
older than this contract) fails the request instead of answering without it.

## Compatibility

- **Version.** The gRPC package is `vmafx.v1`, the HTTP resources live under
  `/v1`. Within `v1` the API only grows: new RPCs, new messages, new fields
  with new numbers, new values of a field's list. Nothing is renamed,
  removed, renumbered or retyped. `buf breaking` (proto) and the definition's
  append-only checker (`python3 scripts/codegen/vmafx-api.py --abi-check`)
  refuse such a change before it merges.
- **Deprecation.** A field to be removed is first marked deprecated in the
  definition, which puts the mark into the generated proto comment, the
  OpenAPI description and the tables on this page. It keeps working for at
  least one minor release after the mark and is removed only in a new major.
- **Breaking changes.** A change that cannot be additive ships as `vmafx.v2`
  next to `v1`; both are served until `v1` is retired with a release note.
- **Defaults.** An unset field means the default in its row. A default the
  library owns (the default model) follows the library and is named, not
  copied, so it moves only with the library's release notes.

## Where the contract comes from

The options and the provenance record are defined once, in the option groups
and structs of `core/api/vmafx.toml`, and generated into every surface
([API generation](../development/api-generation.md#option-groups)):

| Generated file | Surface |
| --- | --- |
| `proto/vmafx_api.proto` | `ScoreOptions` and `Provenance` messages; the services in `proto/vmafx.proto` import them |
| `api/openapi/components.gen.yaml` | The same schemas for OpenAPI clients; spliced into `api/openapi/vmafx-server-v1.yaml` |
| `pkg/scoreopts/options.gen.json` | The flags the server passes to `vmaf` for each option |
| `core/tools/cli_options.gen.inc` | The `vmaf` option table and usage text |

## Option reference

<!-- BEGIN GENERATED: vmafx-api score options (scripts/codegen/vmafx-api.py) -->

| Field | Number | Message | Value | Default when unset | Description |
| --- | --- | --- | --- | --- | --- |
| `width` | 1 | `ScoreOptions` | uint >= 1 | | Width of raw .yuv input in pixels. |
| `height` | 2 | `ScoreOptions` | uint >= 1 | | Height of raw .yuv input in pixels. |
| `pixel_format` | 3 | `ScoreOptions` | `420` \| `422` \| `444` | | Chroma subsampling of raw .yuv input. |
| `bitdepth` | 4 | `ScoreOptions` | `8` \| `10` \| `12` \| `16` | | Bits per sample of raw .yuv input. |
| `disable_clip` | 10 | `ScoreOptions` | bool | `false` | Do not clip the model score to [0, 100] (the model's disable_clip). |
| `enable_transform` | 11 | `ScoreOptions` | bool | `false` | Apply the model's score transform (the model's enable_transform). |
| `backend` | 12 | `ScoreOptions` | `auto` \| `cpu` \| `cuda` \| `sycl` \| `hip` \| `metal` | `auto` | Backend: auto uses the available ones; any other value runs that backend alone and fails when it is not available. |
| `feature` | 22 | `ScoreOptions` | string (list) | | Additional feature extractor, name[=key=value:...] (for example psnr or cambi=full_ref=true); several may be given (the filter separates them with \|). Mutually exclusive with the CTC presets. |
| `aom_ctc` | 23 | `ScoreOptions` | `v1.0` \| `v2.0` \| `v3.0` \| `v4.0` \| `v5.0` \| `v6.0` \| `v7.0` | | AOM common test conditions preset: a fixed model and feature set. |
| `nflx_ctc` | 24 | `ScoreOptions` | `v1.0` | | Netflix common test conditions preset: a fixed model and feature set. |
| `tiny_model` | 30 | `ScoreOptions` | string | | Tiny ONNX model to load alongside the classic models. |
| `tiny_device` | 31 | `ScoreOptions` | `auto` \| `cpu` \| `cuda` \| `openvino` \| `openvino-npu` \| `openvino-cpu` \| `openvino-gpu` \| `coreml` \| `coreml-ane` \| `coreml-gpu` \| `coreml-cpu` \| `rocm` | `auto` | ONNX Runtime execution provider of the tiny model. |
| `tiny_threads` | 32 | `ScoreOptions` | uint | | Intra-op threads of the CPU execution provider (0: the runtime's default). |
| `tiny_fp16` | 33 | `ScoreOptions` | bool | `false` | Request fp16 input and output where the execution provider supports it. |
| `tiny_model_verify` | 34 | `ScoreOptions` | bool | `false` | Require a Sigstore bundle verification of the tiny model (cosign verify-blob) before it loads; a missing bundle, a missing cosign or a failed verification refuses the model. |
| `tiny_codec` | 35 | `ScoreOptions` | string | | Encoder of the distorted clip, required by codec-aware tiny models (fr_regressor_v2/v3), which refuse to score without it. Must be in the model sidecar's encoder_vocab; the ffprobe names h264, hevc, av1, vp9 and vvc are accepted. |
| `tiny_preset` | 36 | `ScoreOptions` | string | | Encoder preset (medium, slow, p4, 5, ...), read as the encoder defines it. Unset: ordinal 5 (medium). A model trained with one preset (fr_regressor_v3) ignores it and warns. |
| `tiny_crf` | 37 | `ScoreOptions` | uint 0..63 | | CRF or QP used for the encode, normalised as the model sidecar declares. Required with the codec and preset. |
| `tiny_resize` | 38 | `ScoreOptions` | `bilinear` \| `nearest` \| `bicubic` \| `disabled` | `disabled` | Resize filter for NCHW tiny models whose input size differs from the frame; disabled refuses the mismatch (-ERANGE). The three filters give scores about 2% apart: record the filter with the model. |
| `no_reference` | 39 | `ScoreOptions` | bool | `false` | No-reference mode; needs a no-reference tiny model. The reference becomes a formality: only the distorted picture is scored. |
| `threads` | 14 | `ScoreOptions` | uint | | Worker threads of the feature extractors, capped to the hardware threads (0: score in the calling thread). |
| `frame_cnt` | 16 | `ScoreOptions` | uint >= 1 | | Score at most this many frames. |
| `frame_skip_ref` | 17 | `ScoreOptions` | uint | | Skip this many frames at the start of the reference. |
| `frame_skip_dist` | 18 | `ScoreOptions` | uint | | Skip this many frames at the start of the distorted video. |
| `no_prediction` | 19 | `ScoreOptions` | bool | `false` | Extract features only; no model score. |
| `cpumask` | 20 | `ScoreOptions` | uint | | Bitmask of CPU instruction sets the extractors must not use. |
| `gpumask` | 21 | `ScoreOptions` | uint | | Bitmask of GPU operations the extractors must not use. |
| `device` | 13 | `ScoreOptions` | string | `auto` | GPU of the selected backend: auto, or a device index. Needs a GPU backend that selects devices by index (sycl, hip, metal). |
| `subsample` | 15 | `ScoreOptions` | uint >= 1 | `1` | Score every n-th frame (1: every frame). |
| `precision` | 40 | `ScoreOptions` | `legacy` \| `max` \| `full` \| `1` \| `2` \| `3` \| `4` \| `5` \| `6` \| `7` \| `8` \| `9` \| `10` \| `11` \| `12` \| `13` \| `14` \| `15` \| `16` \| `17` | `max` | Score precision: N (1 to 17) writes %.&lt;N&gt;g; max or full write %.17g (round-trip lossless); legacy writes %.6f (Netflix-compatible). The scoring server returns lossless scores unless asked otherwise. |
| `view_distance` | 50 | `ScoreOptions` | float 3..24 | | Viewing distance in display heights (ADM adm_norm_view_dist). Unset: the model's value. The ADM extractor's default CSF refuses a distance below 3 (it accepts 0.75 with other CSF modes, which these options do not set). |
| `display_height` | 51 | `ScoreOptions` | uint >= 1 | | Height of the reference display in pixels (ADM adm_ref_display_height). Unset: the model's value. |
| `target_width` | 52 | `ScoreOptions` | uint | `0` | Width of the target display the distorted video is scaled to (0: no scaling). Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `target_height` | 53 | `ScoreOptions` | uint | `0` | Height of the target display the distorted video is scaled to (0: no scaling). Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `target_scaling` | 54 | `ScoreOptions` | `none` \| `bilinear` \| `bicubic` \| `lanczos` | `none` | Scaling filter towards the target display. Reserved: device-targeted scoring lands in RC5; only the default is accepted. |

<!-- END GENERATED: vmafx-api score options -->
