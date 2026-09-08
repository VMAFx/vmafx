- **ADR cross-reference corrections**: 33 inline citations pointed at
  ADR-0461 ("CLI validates positive dimensions and chroma-alignment on
  input videos") for decisions it does not record. The `gpu_dispatch_env`
  once-snapshot caller-contract that every `concurrency-mt-unsafe` NOLINT
  cites is [ADR-0488](docs/adr/0488-gpu-dispatch-env-shared-snapshot.md);
  the `psnr_hvs_vulkan` `enable_chroma` option is
  [ADR-0585](docs/adr/0585-psnr-hvs-vulkan-enable-chroma.md); the
  `float_ms_ssim` `enable_chroma` option is
  [ADR-0583](docs/adr/0583-float-ms-ssim-enable-chroma.md). ADR-0858's
  `## References` also linked `0461-gpu-dispatch-env-centralised-snapshot.md`,
  a file that has never existed. The four citations that genuinely refer to
  ADR-0461's CLI validation are unchanged.
