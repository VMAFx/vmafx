- Docs sweep covering audit-slice-A/B/C residuals after Batches 4/5/15
  landed:
  - `docs/api/dnn.md`: new section documenting
    `vmaf_dnn_verify_signature` (Sigstore-keyless verification primitive)
    + `--tiny-model-verify` CLI coupling, build/platform requirements,
    Windows-`-ENOSYS` carve-out.
  - `docs/usage/bench.md`: corrected the hard-coded resolution list
    (`576x324, 640x480, 1280x720, 1920x1080, 3840x2160` per
    `vmaf_bench.c:291-293`) and the staging-directory example.
  - `docs/usage/ffmpeg.md`: extended the "Fork-added options" table
    with the 5 missing backend selectors (`sycl_device`, `vulkan_device`,
    `cuda`, `hip_device`, `metal_device`) including the `metal_device`
    `-2` default convention.
  - `docs/usage/vmaf-vpl.md` (new): full page for the previously
    undocumented developer tool (8 flags, build prereqs, smoke
    invocation, status).
  - `docs/usage/vmaf-train.md` (new): full reference for the 14-subcommand
    Python training CLI plus two end-to-end workflow examples.
  - `docs/usage/vmaf-roi.md`: added a binary-name note (built and
    installed as `vmaf_roi` underscore form).
  - `docs/ai/konvid-1k-ingestion.md`: corrected stale "Phase 2 not yet
    shipped" claims (corpus + adapter both shipped).
  - `docs/ai/models/fr_regressor_v3.md`: rewrote the lift-floor
    paragraph to drop the "scaffold-only limbo" framing (v3 ships in
    production; the lift floor will be measured retroactively when a
    multi-codec corpus arrives).
  - `docs/ai/models/fr_regressor_v2_probabilistic.md`: removed the
    stale "renumber if PR #347" placeholder (PR #347 merged long ago).
