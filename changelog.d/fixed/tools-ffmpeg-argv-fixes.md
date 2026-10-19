- **The vmaf-tune tools build FFmpeg command lines that do what they say.**
  Repeated `-x265-params` (two-pass stats, saliency zones, HDR SEI) or
  `-x264-params` / `-svtav1-params` / `-vvenc-params` options are joined into one
  (FFmpeg keeps only the last, so a pass-2 encode lost its stats file or its
  zones); the per-shot probe and signalstats passed the shot's start frame
  index to `-ss`, which reads seconds, and now convert it with the frame rate;
  `hevc_nvenc` no longer gets `-master_display` / `-max_cll`, which FFmpeg
  rejects and which aborted the encode; `/dev/null` became `os.DevNull`; the
  saliency check keys on the ROI keys instead of any `-x265-params`. The
  `libvmaf_cuda` recipes convert NVDEC's NV12 with `scale_cuda` (the filter
  accepts `yuv420p` and `yuv444p16` only).
