# vmaf-tune: NVENC + QSV adapter dedup via Base{Nvenc,Qsv}Adapter

Introduced `BaseNvencAdapter` (in `_nvenc_common.py`) and `BaseQsvAdapter`
(in `_qsv_common.py`) as frozen dataclasses that centralise the shared
`ffmpeg_codec_args`, `validate`, `gop_args`, `force_keyframes_args`, and
`probe_args` method bodies common to all three per-family adapters.

Each of the six per-codec adapter files (`h264_nvenc`, `hevc_nvenc`,
`av1_nvenc`, `h264_qsv`, `hevc_qsv`, `av1_qsv`) now inherits from its
respective base and overrides only the `name` / `encoder` fields,
reducing each file from ~80–92 LOC to ~26 LOC. No behaviour change —
all 95 NVENC + QSV adapter tests pass unchanged.

LOC before → after (non-blank, non-comment):

| adapter       | before | after |
|---------------|--------|-------|
| h264_nvenc    |     56 |    10 |
| hevc_nvenc    |     53 |    10 |
| av1_nvenc     |     56 |    10 |
| h264_qsv      |     55 |    10 |
| hevc_qsv      |     52 |    10 |
| av1_qsv       |     56 |    10 |
| _nvenc_common |     43 |    96 |
| _qsv_common   |     52 |   107 |
| **net delta** |  **−81 LOC** | |
