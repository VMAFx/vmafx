
- **SpEED chroma SYCL/CPU parity now holds on every channel.** The per-element
  means were computed on the device and came out one ulp away from the CPU
  reference's, which propagated into all 625 covariance entries. The fixture
  reduces to a 10x10 plane, so the 25x25 covariance is estimated from a 6x6
  submatrix — 36 samples for 25 dimensions — and the near-singular eigen/QR/solve
  amplified that ulp roughly 70x, landing the V channel at 1.45e-4 against a
  1e-4 tolerance. The means are now computed on the host by the CPU reference's
  own routine, exposed as `speed_internal_compute_means` so there is one
  implementation rather than two. They are 1/25th of the covariance work, so
  offloading them bought nothing. `test_sycl_speed_chroma_parity` passes, the
  SYCL suite is 195/195, and the real-video CLI comparison is bit-identical on
  all three chroma scores.
