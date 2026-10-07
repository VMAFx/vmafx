- **The `vmafx` FFmpeg filter no longer scores a reference frame that does not
  belong to the main frame, and no longer fails a run that scored nothing
  ([ADR-2165](docs/adr/2165-vmafx-filter-pairing-contract.md)).** FFmpeg's
  framesync repeats the last frame of a reference that ends early: 24
  reference frames under 48 main frames scored 89.659227 where the paired
  frames score 96.580028, with no message. The filter now defaults to
  `eof_action=pass` and `repeatlast=0`, passes main frames without a reference
  frame on unscored and says how many, warns when the two inputs differ in
  frame rate or when pairs lie more than half a frame apart, and names the
  input order at init. A filter with no scored pair (`enable=` off, empty
  input) used to end the command with exit 187 (`vmafx_flush` on an empty
  context); it now succeeds and writes no report. A `log_path` that cannot be
  written fails at configuration instead of after the last frame. The old
  behaviour is `eof_action=repeat:repeatlast=1`. Checked by
  `ffmpeg-patches/test/vmafx_filter_check.py contract` (six of seven checks
  failed on the previous head).
