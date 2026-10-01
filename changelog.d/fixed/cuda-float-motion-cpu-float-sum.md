- **`float_motion_cuda` returns the CPU's scores bit for bit.** The CPU
  `float_motion` extractor adds the absolute differences of a row into one
  `float`, the row sums into another, and divides in `float`, so its score
  depends on that order. The CUDA twin summed each 16x16 block on the device
  and the blocks in `double` on the host, which left `motion`, `motion2` and
  `motion3` up to 1.36e-4 from the CPU on 1920x1080 checkerboards (above the
  5e-5 cross-backend tolerance), 2.4e-5 at 3840x2160 and 3.1e-6 on the
  Netflix 576x324 pair. It now adds each row on the device in the CPU's
  order and the rows on the host
  ([ADR-1409](docs/adr/1409-float-motion-twins-cpu-float-sum.md)). Measured
  on an RTX 4090 at `--precision max`: every frame identical on the Netflix
  pair, both 1080p checkerboard pairs and 200 frames of BBB 3840x2160, also
  at 10 bits and with the fps-weight, cap and blend options set; the time per
  3840x2160 frame did not change (3.00 and 2.98 ms). The parity gate compares
  this twin with tolerance 0. Stored `float_motion_cuda` outputs change in
  their low digits by at most those differences. The SYCL, HIP and Metal
  twins still agree with the CPU to four decimal places.
