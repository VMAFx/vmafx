- **`y4m_convert_411_422jpeg` 1-byte heap-buffer-overflow on
  4:1:1 streams whose destination chroma row reduces to a single
  pixel (`dst_c_w == 1`).** The Daala-derived 4:1:1 → 4:2:2-jpeg
  chroma upsample in `core/tools/y4m_input.c` runs three sub-loops
  over the destination row; only the third carried the
  `(x << 1 | 1) < dst_c_w` guard around the secondary write. The
  first two sub-loops wrote `_dst[(x << 1) | 1]` unconditionally,
  which is a 1-byte OOB write when `dst_c_w == 1` (and a same-shape
  bug, masked by the loop bounds, in the middle sub-loop). Affects
  the CLI's Y4M ingest path (`vmaf -r foo.y4m`) and the
  `vmaf_pre`/`libvmaf` FFmpeg filters when the upstream pipeline
  hands them a 4:1:1 stream of width 2 — practical heap corruption,
  not just a sanitiser warning. Surfaced by the libFuzzer scaffold
  staged in PR #348 within seconds of corpus startup. Fix mirrors
  the third sub-loop's guard onto the first two; new regression
  test `core/test/test_y4m_411_oob.c` drives the parser through
  `video_input_open` + `video_input_fetch_frame` and is ASan-clean
  post-fix, faulting at `y4m_input.c:507` with `WRITE of size 1`
  pre-fix. Netflix CPU golden tests unaffected (none use 4:1:1 with
  `dst_c_w == 1`).
