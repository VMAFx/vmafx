- **SYCL: the exact SpEED covariance runs as a split chain
  ([ADR-2690](docs/adr/2690-sycl-speed-covariance-fast-exact.md)).** The
  differences and products are formed in parallel and stored as fp64 bit
  patterns, and one sequential add chain per entry replays `speed.c`'s order,
  so `speed_chroma` and `speed_temporal` keep the CPU's bits. On an Arc A380
  with `vmaf_v1.0.16_3d0h` at 3840x1600 the filter takes 23.06 ms of GPU time
  per frame against 28.53 ms for the per-entry exact kernel (56 to 59 % of its
  cost recovered) and 18.83 ms for the old inexact kernel; the remaining
  +4.2 ms is `T-SYCL-SPEED-COV-EXACT-SEQUENTIAL-COST-2026-10-06` (RC8).
