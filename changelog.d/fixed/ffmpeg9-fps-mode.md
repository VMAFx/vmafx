- **ffmpeg calls work with FFmpeg 9 again**: FFmpeg 9 removed the `-vsync`
  option, so the Python harness's decode step and the `describe_worst_frames`
  MCP tool (Python and Go servers) failed with "Unrecognized option 'vsync'"
  on the FFmpeg release this project pins. They now pass
  `-fps_mode passthrough`, the same mode as the old `-vsync 0`. The harness
  change ports Netflix/vmaf `aeaf2877d`.
