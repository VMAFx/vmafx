- ffmpeg: document the `libvmaf` filter input-ordering convention
  (`[0:v]` = distorted / main, `[1:v]` = reference — the OPPOSITE
  of the Python runner and `vmaf` CLI which take `(ref, dis)`).
  Inputs in the wrong order silently inflate the score (e.g. 83.78
  instead of 76.67 on the Netflix golden pair) with no warning.
  Patch 0001 now emits an `AV_LOG_INFO` reminder at filter init.
  See `docs/usage/ffmpeg.md` § Input ordering convention.
  (surfaced by ffmpeg libvmaf smoke test, PR #1557).
