- **docs(adr):** ADR-0767 — Phase 4b.8 C ABI break scoping for VMAFx v4.0.0: nine
  proposed breaking changes (config-by-pointer, remove `vmaf_write_output` /
  `vmaf_model_load`, normalize `void`→`int` returns, SYCL namespace move, add
  `vmaf_context_get_backend`); full ffmpeg-patches lockstep rewrite plan; migration
  guide outline and test plan. Companion Research-0752 contains the complete public
  symbol inventory across all 14 headers and all 15 ffmpeg-patch entry-point callsites.
  No source changes — scoping/design document only. (ADR-0767)
