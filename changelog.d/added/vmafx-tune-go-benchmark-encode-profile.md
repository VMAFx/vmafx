Ported the `benchmark` and `encode-profile` subcommands of `vmaf-tune` to Go in
`vmafx-tune-go`, replacing their loud-fail stubs (ADR-0705 / ADR-0730 /
ADR-0770 staged port). `benchmark` ranks encoders from an existing Phase-A
corpus JSONL at a matched target VMAF in Markdown / JSON / CSV; `encode-profile`
reads the `encoder_profile` payload out of a report JSON, HTML or Markdown
file, selects one recommendation, and reproduces that encode with FFmpeg
(`--dry-run` prints the argv instead), propagating FFmpeg's own exit status.

Three new packages back them — `pkg/benchmark` (corpus summarisation and
renderers), `pkg/codecadapter` (the argv-shaping half of
`vmaftune.codec_adapters`, all 19 codecs) and `pkg/encodeprofile` (profile
loading, recommendation selection, FFmpeg argv composition and the encode
driver) — plus `internal/pyjson`, which renders Go value trees byte-identically
to CPython's `json.dumps(..., indent=2, sort_keys=True)`.

Output parity is verified against the Python implementation rather than
asserted: the float renderer matches CPython `repr()` on 8025 values, the codec
adapters match `encode._resolve_codec_args` across 696 (codec, preset, quality)
cases, and 20 end-to-end CLI invocations plus live x264 / x265 / NVENC encodes
produce byte-identical stdout and identical exit codes.
