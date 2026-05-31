- **chore(changelog.d):** Prune 7 stale `changelog.d/` fragments that
  advertised behaviour no longer on master, and rewrite 2 misleading
  fragments. Vulkan-backend fragments (`vif-arc-mesa-anv-int64-reduction`,
  `vulkan-vif-fp64-g-computation`, `motion-vulkan-1067-regression`) were
  obsoleted by ADR-0726 (Vulkan backend dropped). `float_ansnr` fragments
  (`float-ansnr-enable-chroma-pr947-restore`, `hip-ansnr-memcpy-direction`)
  were obsoleted by the `float_ansnr` extractor removal (PR #38, cited in
  ADR-0749). HIP `dispatch-strategy-pr1067-regression` referenced a
  fork-history `g_hip_features[]` table that never landed on master (the
  HIP dispatch_strategy is still the ADR-0212 stub). `metal-dispatch-strategy-key-restore`
  was superseded by the canonical-score-names fix on master
  (commit ff4c30d40c). `onnx-blobs-to-github-releases` was rewritten to
  reflect the actual scaffold-only state (the fetcher script exists but
  the 3 large blobs are still inlined in git pending the
  `tiny-blobs-v1` Release upload). `cuda-extractor-cambi-and-ssim-promotion`
  was rewritten to drop the contradiction with the cambi_cuda SIGSEGV
  fixes that landed in PR #866 + PR #870.
