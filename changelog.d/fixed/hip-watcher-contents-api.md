- CI: the FFmpeg HIP hwdec watcher reads the FFmpeg mirror through the contents API instead of
  code search. Code search is throttled for the workflow token, and dispatch run 39160669868 still
  failed on a `search/code` 429 after three attempts with the stated-delay wait. The watcher now
  lists `libavutil/` for `hwcontext_hip.{c,h}` and decodes `libavutil/hwcontext.h` for an
  `AV_HWDEVICE_TYPE_HIP` or `_ROCM` variant (`T-HIP-WATCHER-CODE-SEARCH-429-2026-10-18`).
