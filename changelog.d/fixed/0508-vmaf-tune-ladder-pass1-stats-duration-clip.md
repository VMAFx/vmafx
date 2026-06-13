- `vmaf-tune ladder --duration N` now clips the **entire** encode
  pipeline (pass 1 + pass 2). The ADR-0506 V6-1 fix wired the
  duration into `build_ffmpeg_command` (covering single-pass
  `run_encode` and Phase-F `run_two_pass_encode`), but the
  sibling `build_pass1_stats_command` — taken by every codec
  adapter that declares `supports_encoder_stats = True`
  (`libx264` in particular, the corpus default) — still emitted
  pass-1 stats over the full source. Net effect: a
  `ladder --duration 5` smoke run against a 10-minute BBB source
  still burned ~10 min of wall time per cell on the pass-1
  stats sweep before pass 2 ran on the requested 5-second
  window. `build_pass1_stats_command` now mirrors the V6-1
  fallback (input-side `-t duration_s` when the caller did not
  opt into sample-clip mode); `sample_clip_seconds` keeps
  precedence (ADR-0508, Bug #V8-A).
