- **`speed_temporal` no longer overruns its frame buffers when `speed_prescale`
  is above 1.** The CPU extractor sized its four frame buffers with the source
  height, but resamples each frame in place at the prescaled height, so
  `--feature speed_temporal=speed_prescale=1.5` read and wrote past the end of
  the allocation: an AddressSanitizer build reports a heap-buffer-overflow and a
  release build aborts with `free(): invalid size` or corrupts the heap. The
  buffers now hold the upscaled plane, as `speed_chroma`'s already did. Scores
  at `speed_prescale` 1 and below are unchanged, and the CUDA, SYCL and HIP
  twins were not affected. Reported upstream as
  [Netflix/vmaf#1626](https://github.com/Netflix/vmaf/issues/1626)
  ([features](docs/metrics/features.md#options-shared)).
