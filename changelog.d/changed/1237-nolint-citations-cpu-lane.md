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
  justification". (ADR-0141, ADR-0278)
