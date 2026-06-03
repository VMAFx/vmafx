- `vmaf-tune encode` x264/x265/libvpx-vp9 encoder capability is now
  detected from the `ffmpeg -version` configure summary (`--enable-*`
  flags) and surfaced through the new `EncoderInfo.codec_detected` bool
  field returned by `probe_encoder_info`. The existing `libx264` and
  `libsvtav1` patterns are joined by `libx265` and `libvpx-vp9`
  (ADR-0498 follow-up #7).
- `build_pass1_stats_command` had a duplicate `fallback_duration`
  assignment (dead first write left by the #1266 refactor). The
  duplicate is removed; the duration-s clamp still works correctly
  (ADR-0498 follow-up #7, Bug #V8-A cleanup).
- `vmaf-tune fast` TPE proxy-encode trials now score on the same GPU
  backend as the mandatory verify pass. Previously all 30 TPE probe
  scores ran on CPU even when a GPU backend was available; the backend
  selected by `score_backend.select_backend` is now forwarded to
  `_build_production_sample_extractor` (ADR-0498 follow-up #7).
- `codec_adapters.parse_available_codecs` parses `ffmpeg -hide_banner
  -encoders` output into a `frozenset` of available codec names.
  Callers can gate stats-capture or hardware-path logic on runtime
  codec availability rather than compile-time assumptions
  (ADR-0498 follow-up #7).
