- Reuse integer VIF AVX2 and AVX-512 stage-test fixtures per geometry and build
  their shared logarithm table once, keeping all 3,456 scalar/SIMD comparisons
  within the Coverage Gate's per-test timeout.
