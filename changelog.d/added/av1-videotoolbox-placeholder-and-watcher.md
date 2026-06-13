- **`av1_videotoolbox` placeholder codec adapter + upstream watcher
  ([ADR-0339](../docs/adr/0339-av1-videotoolbox-placeholder-adapter.md)).**
  Apple M3 / M4 silicon has hardware AV1 encode capability but
  FFmpeg upstream has not yet exposed it (verified against
  `git.ffmpeg.org/ffmpeg.git` master `8518599cd1`, 2026-05-09:
  `libavcodec/videotoolboxenc.c` registers H264 / HEVC / PRORES
  only). New `Av1VideoToolboxAdapter` registers in
  `tools/vmaf-tune/src/vmaftune/codec_adapters/` with
  `supports_runtime=False` and raises
  `Av1VideoToolboxUnavailableError` from `validate` /
  `ffmpeg_codec_args` until a runtime probe of `ffmpeg -h
  encoder=av1_videotoolbox` confirms the encoder is live — at which
  point the adapter self-activates with no code change. Paired with
  `scripts/upstream-watcher/check_ffmpeg_av1_videotoolbox.sh` and a
  weekly cron at `.github/workflows/upstream-watcher.yml`
  (Mondays 08:00 UTC) that opens an `upstream-blocked` GitHub issue
  the moment FFmpeg upstream lands the encoder. First member of the
  upstream-watcher pattern documented in
  [`docs/development/upstream-watchers.md`](../docs/development/upstream-watchers.md).
  Tests under `tools/vmaf-tune/tests/test_codec_adapter_av1_videotoolbox.py`
  cover both placeholder-mode (raises `Av1VideoToolboxUnavailableError`)
  and post-activation (correct argv emission) via an injected
  subprocess runner — no real FFmpeg invocation, no macOS
  dependency.
