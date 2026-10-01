- **`speed_chroma_cuda` and `speed_temporal_cuda` match the CPU with
  `speed_prescale_method=lanczos4`.** The CPU scaler evaluates each lanczos4
  weight in fp64 with `sin()` and rounds it once; the CUDA scale kernel
  evaluated the weights itself in fp32, a few ulp off on some of them, and
  SpEED amplifies that on smooth content. On an RTX 4090 the twins were up to
  8.8e-3 relative (0.27 absolute) from the CPU on a smooth synthetic field and
  1.0e-3 absolute on a 1920x1080 gradient with noise, beyond the cross-backend
  tolerance of 1e-4. The weights depend only on the output column and row, so
  the host now evaluates them once per run with the scaler's own routine and
  the kernel reads the table: nearest, bilinear, bicubic and lanczos4 prescale
  at 0.5 and 2.0 are all bit-identical to the CPU extractor. Scores with the
  other three methods, and without prescale, are unchanged. The SYCL and HIP
  twins still evaluate the weights on the device
  ([SpEED](docs/metrics/speed_qa.md#cuda-the-same-chain-on-the-device)).
