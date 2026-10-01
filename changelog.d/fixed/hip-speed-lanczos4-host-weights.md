- **`speed_chroma_hip` and `speed_temporal_hip` match the CPU with
  `speed_prescale_method=lanczos4`.** Like the CUDA and SYCL twins before
  them, they evaluated the lanczos4 kernel weights on the device in fp32,
  where the CPU scaler uses fp64 `sin()`, and SpEED amplifies the few-ulp
  differences on smooth content: on a gfx1036, `speed_chroma_hip` at
  `speed_prescale=0.5` was 8.8e-3 relative away from the CPU on a smooth
  1920x1080 field, and `speed_temporal_hip` 0.24 on a 1080p checkerboard pair
  at 2.0. The scale kernel now reads the weights from the table the host
  builds with the CPU scaler's own routine, and lanczos4 at 0.5 and 2.0 is
  bit-identical to the CPU extractor on that device (given a correctly rounded
  `log2f` on the CPU side, as for the other prescale methods). It is also
  faster: `speed_chroma_hip` with lanczos4 at 0.5 goes from 21.8 to 16.0 ms
  per 3840x2160 frame. Scores with the other three methods, and without
  prescale, are unchanged. This closes the lanczos4 prescale drift on all
  three device backends
  ([SpEED](docs/metrics/speed_qa.md#hip-device-resident-cpu-fp32-arithmetic)).
