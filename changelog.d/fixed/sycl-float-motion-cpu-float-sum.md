- **`float_motion_sycl` returns the CPU's scores bit for bit.** The SYCL twin
  summed the absolute differences of each 32x4 work-group on the device and
  the groups in `double` on the host, where the CPU `float_motion` extractor
  keeps one `float` running sum per row and one over the rows. Measured on an
  Arc A380 that left `motion` and `motion2` up to 1.36e-4 from the CPU on
  1920x1080 checkerboards (above the 5e-5 cross-backend tolerance), 2.4e-5 at
  3840x2160 and 3.1e-6 on the Netflix 576x324 pair. The twin now adds each
  row on the device in the CPU's order, one work-item per row, and the host
  adds the rows
  ([ADR-1411](docs/adr/1411-sycl-float-motion-cpu-float-sum.md), after
  ADR-1409 for CUDA). At `--precision max` every frame is identical on the
  Netflix pair, both 1080p checkerboard pairs and 200 frames of BBB
  3840x2160, also at 10, 12 and 16 bits and with `motion_fps_weight` and
  `motion_max_val` set. The second pass over the blurred planes costs 0.38 ms
  per 3840x2160 frame on the A380 (3.85 to 4.23 ms). The parity gate compares
  this twin with tolerance 0. Stored `float_motion_sycl` outputs change in
  their low digits by at most those differences. The HIP and Metal twins
  still agree with the CPU to four decimal places.
