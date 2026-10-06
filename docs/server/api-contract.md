# Scoring API contract

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
| `view_distance` | 50 | `ScoreOptions` | float 0.75..24 | | Viewing distance in display heights (ADM adm_norm_view_dist). Unset: the model's value. |
| `display_height` | 51 | `ScoreOptions` | uint >= 1 | | Height of the reference display in pixels (ADM adm_ref_display_height). Unset: the model's value. |
| `target_width` | 52 | `ScoreOptions` | uint | `0` | Width of the target display the distorted video is scaled to (0: no scaling). Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `target_height` | 53 | `ScoreOptions` | uint | `0` | Height of the target display the distorted video is scaled to (0: no scaling). Reserved: device-targeted scoring lands in RC5; only the default is accepted. |
| `target_scaling` | 54 | `ScoreOptions` | `none` \| `bilinear` \| `bicubic` \| `lanczos` | `none` | Scaling filter towards the target display. Reserved: device-targeted scoring lands in RC5; only the default is accepted. |

<!-- END GENERATED: vmafx-api score options -->
