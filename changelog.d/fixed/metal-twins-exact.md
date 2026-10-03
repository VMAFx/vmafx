- **The Metal twins compute the CPU's scores by construction
  ([ADR-1498](docs/adr/1498-metal-twins-exact-designs.md)).** Each Metal
  extractor now runs the design that makes its CUDA, HIP or SYCL twin return
  the CPU's scores bit for bit, written in a header that also compiles on the
  host, where a test holds it against the CPU extractor. Changes a Metal user
  can see: `float_psnr_metal` and `float_moment_metal` are exact at 10 to 16
  bits, `float_moment_metal` also on 16-bit frames whose sum passes 2^53
  units; `integer_psnr_metal` no longer loses carries in its 64-bit error sum
  and takes `enable_apsnr`; `integer_motion_metal` differences frames before
  the blur, emits `motion_sad_score` and `motion3` and drops the
  `motion_add_uv` option the CPU never had; `motion_v2_metal` and
  `float_motion_metal` apply `motion_fps_weight` and `motion_max_val` per frame
  and take every CPU option; `integer_vif_metal` hands frames below 16 pixels
  to the CPU in a model run; `float_adm_metal` refuses frames below 17x17,
  floors its sums as the CPU and takes `adm_f1s0` to `adm_f2s3`;
  `float_vif_metal` runs every `vif_kernelscale`; `float_ssim_metal` gives an
  identical flat frame a finite `enable_db` score; `integer_cambi_metal` takes
  `src_width`, `src_height` and `full_ref`. No Apple device ran these builds
  yet: each twin's state row closes when a report of the macOS tester bundle
  shows its parity test passing. Guide: `docs/backends/metal/index.md`.
