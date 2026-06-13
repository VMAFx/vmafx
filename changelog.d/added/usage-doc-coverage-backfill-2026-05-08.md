- **Usage-doc coverage audit + topic-tree backfill
  ([Research-0086](../docs/research/0086-usage-doc-coverage-audit-2026-05-08.md)).**
  Scanned all 255 in-tree ADRs against the
  [`CLAUDE.md` §12 r10](../CLAUDE.md) doc-substance rule. 46 GOOD /
  31 BACKFILL / 178 N/A. Added five highest-leverage usage docs in
  full prose: [`vmaf-tune-codec-adapters.md`](../docs/usage/vmaf-tune-codec-adapters.md)
  (matrix of all 16 codec adapters per ADR-0288 / 0290 / 0281 /
  0282 / 0283 / 0285 / 0294 / 0279 / 0297),
  [`vmaf-tune-score-backend.md`](../docs/usage/vmaf-tune-score-backend.md)
  (CUDA / Vulkan / SYCL / CPU score paths per ADR-0299 / ADR-0314),
  [`vmaf-tune-cache.md`](../docs/usage/vmaf-tune-cache.md)
  (content-addressed encode/score cache per ADR-0298),
  [`docs/api/vulkan-image-import.md`](../docs/api/vulkan-image-import.md)
  (`vmaf_vulkan_import_image` zero-copy C API per ADR-0184 /
  ADR-0186), and
  [`vmaf-tune-hdr-and-sampling.md`](../docs/usage/vmaf-tune-hdr-and-sampling.md)
  (HDR detection / forcing + clip sampling per ADR-0300 / ADR-0301).
  Added 14 stub docs that cite the relevant ADR for the remaining
  BACKFILL items (vmaf-tune coarse-to-fine, resolution-aware,
  saliency-aware, ladder default sampler, fast-path prod wiring,
  Phase E ladder; SSIMULACRA 2 + DISTS topic-tree entry points; MCP
  release channel; ensemble training kit; `vmaf_tiny_v5` deferral
  record; FFmpeg patches refresh; OSSF Scorecard; tiny-AI per-PR
  doc bar). All new docs are mkdocs-strict-clean and
  markdownlint-clean. ADR bodies untouched per
  [ADR-0028](../docs/adr/0028-adr-maintenance-rule.md).
