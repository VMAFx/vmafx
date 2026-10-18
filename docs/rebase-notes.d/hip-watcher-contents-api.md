## FFmpeg HIP hwdec watcher reads the contents API (2026-10-18)

- `.github/workflows/upstream-ffmpeg-hip-hwdec-watcher.yml` counts its two signals from
  `repos/FFmpeg/FFmpeg/contents/libavutil` and `.../libavutil/hwcontext.h`; no `search/code` call.
- `test_no_workflow_uses_code_search` in `scripts/ci/tests/test_upstream_watch_count.py` refuses a
  code-search call or hint in any watcher workflow.
- No rebase impact beyond this workflow and its test.
