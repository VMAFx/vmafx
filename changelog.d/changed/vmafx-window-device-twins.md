- **Window scores are checked on imported device frames.** The CUDA, SYCL and
  HIP import bit-exactness tests open windows on the imported context before
  any frame is submitted and require each completed window to equal the
  synchronous pooled values of the device and host contexts bit for bit, on
  every exact-twin cell (#2471, ADR-2074).
