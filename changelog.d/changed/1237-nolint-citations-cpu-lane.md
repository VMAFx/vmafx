- **refactor(lint):** Every `NOLINT` directive in the CPU lane
  (`core/src`, `core/tools`, `core/test`, excluding the CUDA / SYCL / HIP /
  Metal backend trees) now carries an inline `ADR-NNNN` citation in the
  ADR-0278 format required by ADR-0141 §2. `scripts/ci/tidy-ratchet.py`'s
  citation rule counts **94 → 0** uncited markers across those directories.
  Cite-only: no function was split, no suppression was widened or removed,
  and no numeric path changed. Two stale references in the SSIMULACRA2 host
  XYB SIMD kernels named ADR-0242 (tiny-AI Netflix training corpus) where
  they meant ADR-0252 (host XYB SIMD); both are corrected. Three prose-only
  mentions of the word "NOLINT" (`core/src/log.cpp`,
  `core/src/mcp/dispatcher.c`, `core/test/test_iqa_helpers.c`) that the
  ratchet regex counted as bare markers are reworded to "suppression
  justification". Four `NOLINTNEXTLINE(readability-function-size, …)`
  directives in the PSNR-HVS and SSIMULACRA2 host SIMD kernels
  (`core/src/feature/{x86,arm64}/`) keep their justification on one line: a
  wrapped directive applies to the following comment rather than to the
  function and silently un-suppresses the diagnostic.
  `scripts/ci/tidy-baseline-cpu.json` is tightened to the measured
  `total_nolint_uncited: 0`; its `warnings` map is untouched. (ADR-0141,
  ADR-0278, ADR-1142)
