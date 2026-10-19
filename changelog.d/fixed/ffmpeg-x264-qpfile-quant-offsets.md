- **`-qpfile` works on libx264, and the saliency tools no longer run a libx264
  encode without the ROI they asked for
  ([ADR-2167](docs/adr/2167-ffmpeg-x264-qpfile-quant-offsets.md)).** Patch
  `0007` gave the libx264 wrapper a `-qpfile` option that called
  `x264_param_parse(.., "qpfile", ..)`; libx264 has no such parameter (the x264
  command line reads the file), so the encoder never opened. The option now
  reads the file and applies each frame's per-macroblock QP offsets through
  x264's `quant_offsets` (+12 on every macroblock: 4186 bytes, none: 20123,
  -12: 104468 on six 576x324 frames), and fails at open, naming why, when
  adaptive quantization is off (`-preset ultrafast` turns it off), the block
  grid is not the video's macroblock grid or the file is malformed. The Go
  and Python saliency code passed `-x264-params qpfile=`, which FFmpeg only
  warns about (`Error parsing option`) before encoding without the ROI; they
  now pass `-qpfile`, which stock FFmpeg refuses. The saliency tests'
  encode-runner stub no longer records the `ffmpeg -version` probe as the
  encode (two tests failed on that, depending on the order they ran in).
