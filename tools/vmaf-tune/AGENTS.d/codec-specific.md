---
paths:
  - tools/vmaf-tune/src/vmaftune/codec_adapters/*.py
  - tools/vmaf-tune/tests/test_codec_adapter_*.py
invariant: QSV shares _qsv_common; VideoToolbox shares _videotoolbox_common; AMF preset compression 7-into-3.
---
<!-- markdownlint-disable MD024 -->
# Specific codec adapter implementations

- **QSV device chain has one implementation and every QSV encode
  uses it (ADR-0601).** `_qsv_common.qsv_device_init_args()` (also
  `BaseQsvAdapter.hw_pre_input_args()` / `qsv_hw_init_args()`) returns
  `-init_hw_device vaapi=va:<node> -init_hw_device qsv=qsv_dev@va
  -filter_hw_device qsv_dev`, and `BaseQsvAdapter.hw_upload_filter` is
  `QSV_UPLOAD_FILTER`. `encode.build_ffmpeg_command` puts first
  before `-i` and appends second to request's own `-vf` chain
  (`encode.with_upload_filter`; second `-vf` would drop rung's
  scale); `compare._hw_probe_argv` uses same two. filter device
  is QSV device: with `va` upload produced `vaapi` frames and
  filter graph failed (A380, iHD driver). new hardware family that
  needs device implements `hw_pre_input_args` / `hw_upload_filter` on
  its adapter instead of branch in `encode.py` or `compare.py`. Go
  side (`pkg/hwdevice`) must emit identical chain.
  `tests/test_qsv_encode_chain.py` pins argv, merge and resolution.
- **AMF's rate-control block lives in `ffmpeg_codec_args` only.**
  `_AMFAdapterBase.extra_params()` returns `()`; it used to repeat
  `-quality / -rc / -qp_i / -qp_p` block, so every AMF command line
  carried it twice (`adapter_version` "2" marks change).
  `encode._resolve_codec_args` calls `extra_params()` without arguments
  for every adapter.
- **`libvpx-vp9` two-pass is FFmpeg-generic, encoder-stats is not.**
  Adapter may set `supports_two_pass = True` because FFmpeg's
  libvpx wrapper honours `-pass` / `-passlogfile`, but
  `supports_encoder_stats` stays `False`: VP9 first-pass stats are
  binary libvpx packet stream, not x264/x265 text stats schema
  consumed by `encoder_stats.py`.
- **`PRESET_NAME_TO_INT` in `codec_adapters/svtav1.py` is closed and
  order-stable** (ADR-0294). Mapping (`placebo`→`0`, `slowest`→`1`,
  `slower`→`3`, `slow`→`5`, `medium`→`7`, `fast`→`9`, `faster`→`11`,
  `veryfast`→`13`) is exercised by every corpus row that records
  `encoder == "libsvtav1"`. Adding name is schema bump for any
  fr_regressor_v2 corpus that pinned previous mapping; reordering
  silently changes integer SVT-AV1 receives. Editing this table
  requires same-PR doc + ADR update.
- **AMF preset compression is fixed (ADR-0282).** 7-into-3 preset
  table in `codec_adapters/_amf_common.py` (`_PRESET_TO_AMF`) is
  cross-codec axis Phase B / C consumers depend on. Do not extend
  `presets` beyond canonical 7 names without amending ADR-0282 —
  registry uniformity that lets search loop ignore codec identity
  rests on every codec accepting same preset vocabulary. AV1
  (`av1_amf`) is RDNA3+ only; `ensure_amf_available` is runtime
  gate.

- **QSV adapters share `_qsv_common.py`.** Three encoders with
  identical parameter shape (preset vocabulary, ICQ
  `global_quality` window) is deliberate exception to "one file
  per codec, nothing shared" Phase A convention. Per ADR-0281,
  future codec families that share parameter shape (NVENC's three
  encoders, AMF's three encoders, VideoToolbox's two H.264 + HEVC
  encoders) follow same pattern: one `_<family>_common.py` private
  module, thin dataclass adapters. Single-codec families stay
  flat.
- **Apple VideoToolbox adapters share `_videotoolbox_common.py`
  (ADR-0283 + ADR-0283 *Status update 2026-05-09*).** Three
  encoders (`h264_videotoolbox`, `hevc_videotoolbox`,
  `prores_videotoolbox`) reuse nine-name preset → `-realtime`
  boolean mapping. H.264 and HEVC share single `-q:v` 0..100
  quality knob (higher = better; `invert_quality=False`). ProRes
  uses `-profile:v` instead — it is fixed-rate intermediate codec,
  so harness's `crf` slot carries integer tier id (0=`proxy` →
  5=`xq`); adapter has its own validator
  `validate_prores_videotoolbox()` and integer-id-to-FFmpeg-alias
  helper `prores_profile_name()`. Per codec-adapter contract,
  search loop never branches on adapter identity — it consumes
  `quality_range` + `ffmpeg_codec_args(...)` uniformly. AV1
  hardware encoding is intentionally absent — Apple Silicon has
  no AV1 hardware encoder block as of 2026 and FFmpeg exposes no
  `av1_videotoolbox`. Tests mock `subprocess.run`; suite runs on
  Linux CI without macOS. End-to-end VT exercise left to
  contributors with macOS + VideoToolbox available locally
  (ProRes additionally requires M1 Pro / Max / Ultra or later —
  Intel Macs with T2 do not have ProRes hardware block).
