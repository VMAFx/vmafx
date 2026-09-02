Add `vmaf-tune compare` runtime variants via `ADAPTER@VARIANT` encoder tokens,
`--encoder-ffmpeg-bin TOKEN=PATH`, and compare-row provenance fields
(`adapter`, `runtime_variant`, `ffmpeg_bin`) so mainline SVT-AV1 and
SVT-AV1-HDR-linked FFmpeg builds can be compared side by side without fake
codec adapters.
