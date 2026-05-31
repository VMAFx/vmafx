### NOLINT citation audit + closeout (ADR-0278 compliance)

All 54 previously uncited `// NOLINT` and `// NOLINTNEXTLINE` suppressions
across `core/` have been given inline citations per ADR-0278 / CLAUDE §12 r12.
No suppressions were added or removed; only inline citations were added.

Two citation patterns applied:
- `misc-use-internal-linkage` on `VmafFeatureExtractor` registration structs:
  `— load-bearing: registry linkage invariant (ADR-0254)`
- `readability-function-size` on upstream-mirror and SIMD kernels:
  `— ADR-0141 upstream-parity` or `— ADR-0141 bit-exactness invariant`

Total NOLINT count before and after: 180 (unchanged).
