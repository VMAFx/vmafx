- `vmaf-tune ladder --duration N` now actually bounds the
  ffmpeg encode pipe to the first N seconds of the source.
  Previously the flag was used only for kbps math, so a 10-second
  smoke run against a 9-minute container source re-encoded the
  full 9 min at every CRF in the sweep (the BBB e2e v6 probe ate
  ~10 min wall time before timing out). `EncodeRequest` gains a
  `duration_s` field that the encode driver translates to an
  input-side `-t duration_s` when the caller has not opted into
  sample-clip mode; `iter_rows` plumbs `CorpusJob.duration_s`
  into it (ADR-0506, Bug #V6-1).
- `vmaf-tune ladder` cross-resolution rungs against a raw-YUV
  source now score successfully on every rung. The v4 scale-
  on-reference-decode path called ffmpeg with `-i src.yuv -f
  rawvideo …` — no input-side demuxer flags — so ffmpeg refused
  the input and the sampler reported "default sampler produced
  no scorable encodes" on every rung whose target dims differed
  from the source's. `_decode_source_to_yuv` now synthesises the
  demuxer-side `-f rawvideo -pix_fmt -s WxH -r FR` block before
  `-i` when the source is raw YUV (ADR-0506, Bug #V6-2).
- `vmaf-tune ladder` returns exit code 2 (operational failure)
  when the sampler raises `RuntimeError`, `ValueError`, or
  `OSError`. The historic path let the exception escape and the
  CLI wrapper still returned 0 to the shell, defeating CI gates
  and shell-script error handling. `_run_ladder` now wraps
  `build_and_emit` in try/except and prints the exception
  message to stderr before returning 2 (ADR-0506, Bug #V6-3).
