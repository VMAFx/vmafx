- **Every SYCL feature kernel now does fp32 arithmetic the way the CPU
  reference does (ADR-1367).** The SYCL guides said the kernels ran in IEEE-754
  strict mode under `-fp-model=precise`; in fact icpx still fused
  `a * b + c` into one FMA and computed `/` and `sqrt` approximately (29% and
  8% of random fp32 operands differed from the host). Every SYCL feature
  translation unit now compiles with
  `-fp-model=precise -ffp-contract=off -foffload-fp32-prec-div -foffload-fp32-prec-sqrt`,
  and the link carries the precision flags for the SPIR-V image that devices
  outside `sycl_icpx_aot_targets` compile at first launch. Nine twins' scores
  move, each still inside its cross-backend tolerance: `float_adm_sycl` is ten
  times closer to `--backend cpu` (2.5e-5 -> 2.5e-6 on the Netflix pair),
  `float_ssim_sycl` and `integer_ssim_sycl` about twice as close, and
  `ciede_sycl` halves its worst 3840x2160 difference (9.7e-5 -> 4.5e-5);
  `float_vif_sycl`, `float_ms_ssim_sycl`,
  `float_motion_sycl` and `vif_sycl` move within their existing spread, and
  `psnr_hvs_sycl` by at most 1.3e-6 dB. The
  twins that were bit-identical to the CPU stay so, and no twin's 4K cost on
  an Arc B580 changed by more than the run-to-run spread. AdaptiveCpp builds
  keep contraction-off only. See
  [the SYCL backend guide](docs/backends/sycl/overview.md#what-the-sycl-compile-line-guarantees).
