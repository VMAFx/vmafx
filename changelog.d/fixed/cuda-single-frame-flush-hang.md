- CUDA: `vmaf --backend cuda --frame_cnt 1` no longer hangs. The engine drains an
  extractor with `while (!(err = fex->flush(...)))`, so a flush that keeps
  returning 0 never terminates; the CUDA motion twin's single-frame back-fill
  returned the append result instead of 1. It now appends at most once and then
  reports "nothing more to append". Single-frame CUDA scores match the CPU on
  every v0.6.1 feature.
