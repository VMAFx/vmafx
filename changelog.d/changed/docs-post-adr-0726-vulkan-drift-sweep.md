- **Docs — post-ADR-0726 Vulkan drift sweep.** `docs/index.md`,
  `docs/metrics/features.md`, `docs/development/build-flags.md`, and
  `docs/api/index.md` still described Vulkan as a production backend
  with a full GPU column. Replaced the Vulkan rows / column entries
  with explicit ADR-0726 removal notes. Also corrected the HIP row in
  `docs/index.md` (8/11 → 7/10) and the `enable_float` build-flag row
  (removed `float_ansnr` per ADR-0720). The historical "Vulkan
  footnotes" in `docs/metrics/features.md` are kept for traceability
  against the original ADR chain, prefixed with a header note that
  explains the removal.
