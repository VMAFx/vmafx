- **`speed_chroma_sycl` and `speed_temporal_sycl` match the CPU with
  `speed_prescale_method=lanczos4`.** Like the CUDA twins before them, they
  evaluated the lanczos4 kernel weights on the device in fp32, where the CPU
  scaler uses fp64 `sin()`, and SpEED amplifies the few-ulp differences on
  smooth content: on an Arc A380 no frame of a 1920x1080 gradient matched at
  `speed_prescale=2.0`. The scale kernel now reads the weights from the table
  the host builds with the CPU scaler's own routine, and nearest, bilinear,
  bicubic and lanczos4 prescale at 0.5 and 2.0 are bit-identical to the CPU
  extractor on that device. The kernels use no scratch memory, so the result
  also holds under the Linux xe driver, and the xe scratch warning no longer
  names the two SpEED twins. Scores with the other three methods, and without
  prescale, are unchanged. The HIP twins still evaluate the weights on the
  device ([SpEED](docs/metrics/speed_qa.md#sycl-device-resident-and-bit-identical-to-the-cpu)).
