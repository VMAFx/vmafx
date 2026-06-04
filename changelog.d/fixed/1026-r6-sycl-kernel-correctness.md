- **SYCL VIF**: fix rd-downsample stride truncation for odd frame widths —
  `e_w / 2` → `(e_w + 1) / 2`; both the scalar and SIMD-16 kernel variants
  are fixed; previously caused OOB writes into the next row for any odd-width
  input (ADR-1026, `r6-sycl-kernel`)
- **SYCL motion/VIF/ADM collect**: propagate `vmaf_sycl_graph_wait()` return
  value in all three collect functions; previously discarded, causing stale
  accumulator reads and wrong scores on device fault/reset (ADR-1026,
  `r6-sycl-kernel`)
