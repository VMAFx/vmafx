- **`float_vif_cuda` returns the CPU's scores bit for bit.** The CUDA twin
  filtered with a table of Gaussian taps that the CPU `float_vif` extractor
  stopped using when it began to compute its filters at start-up (26 of the
  34 taps differ in the last digits), called the device `log2f` where the
  CPU evaluates a polynomial, kept `vif_sigma_nsq` in `float` where the CPU
  keeps it in `double`, and summed per 16x16 block where the CPU adds row by
  row in `float`. That left `vif_scale0..3` up to 3.8e-5 from the CPU on the
  Netflix 576x324 pair and 7.0e-6 at 3840x2160, with no frame identical. The
  twin now takes the taps from the CPU's own routine, evaluates the CPU's
  per-pixel statistic in its types, and adds the terms of each row on the
  device and the rows on the host in the CPU's order
  ([ADR-1412](docs/adr/1412-cuda-float-vif-cpu-arithmetic.md)). Measured on
  an RTX 4090 at `--precision max`: every output of every frame identical on
  the Netflix pair at 8, 10, 12 and 16 bits, both 1080p checkerboard pairs
  and BBB 3840x2160, also with `debug=true` and with non-default
  `vif_enhn_gain_limit`, `vif_sigma_nsq` and `vif_skip_scale0`. The parity
  gate compares this twin with tolerance 0. `float_vif_cuda` also accepts
  the CPU's per-scale floors `vif_scale1_min_val`, `vif_scale2_min_val` and
  `vif_scale3_min_val`. A run of the twin alone takes the same time per
  3840x2160 frame (1.97 and 1.96 ms); its kernels take 1.00 ms instead of
  0.72 ms. Stored `float_vif_cuda` outputs change in their low digits by at
  most 3.8e-5. The SYCL, HIP and Metal twins still agree with the CPU to four
  decimal places.
