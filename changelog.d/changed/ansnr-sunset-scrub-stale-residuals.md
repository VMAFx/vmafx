- **Scrub stale residual ANSNR references across code and comments (epic #1241, ADR-0865)**:
  Cleaned residual comments and docstrings in `ai/data/feature_extractor.py`,
  `core/src/feature/feature_extractor.cpp`, `core/src/feature/offset.c`,
  `core/src/feature/x86/moment_avx2.c`, `core/src/hip/kernel_template.h`,
  `core/test/test_hip_smoke.c`, and `mcp-server/vmaf-mcp/tests/test_p1_tools.py`.
  Corrected historical ADR citations in `docs/metrics/ansnr.md` from ADR-0709 to ADR-0865
  while preserving the page as a concise deprecation stub pointing callers to PSNR and
  PSNR-HVS. Deliberately preserved load-bearing backward-compatibility stubs in
  `compat/python-vmaf/core/quality_runner.py` (`VmafLegacyQualityRunner`) and active negative
  dispatch tests in `core/test/test_metal_kernel_coverage_audit.c` (`ansnr_metal` in phantom list).
