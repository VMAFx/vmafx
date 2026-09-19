# tools/vmaf-roi-score — agent notes

Parent: [../../AGENTS.md](../../AGENTS.md).

## What this directory is

Option C implementation, region-of-interest VMAF *scoring* — drives
`vmaf` CLI twice (full-frame + saliency-masked), blends pooled scores.
Pure Python; no libvmaf C-side changes. See
[ADR-0296](../../docs/adr/0296-vmaf-roi-saliency-weighted.md) and
[ADR-0424](../../docs/adr/0424-vmaf-tune-corpus-benchmark.md).

> **Naming guard**: do **not** rename this tool to `vmaf-roi`. Name
> belongs to `core/tools/vmaf_roi.c` (ADR-0247), encoder-steering
> sibling emitting per-CTU QP-offset sidecars. Different surface,
> different output, related model. Confusing two would silently break
> downstream encoder pipelines.

## Rebase-sensitive invariants

- **None.** `tools/vmaf-roi-score/` wholly fork-local. No upstream
  Netflix/vmaf surface owns or interacts with this directory; upstream
  sync cannot conflict here.
- Combine math (`blend_scores`) = pure linear blend on Python `float`.
  Tests pin endpoints (`w=0` / `w=1`) and midpoint. Changing math =
  schema-version bump (`SCHEMA_VERSION` in
  `src/vmafroiscore/__init__.py`) plus ADR-0296/0424 supersession.
- JSON output schema pinned by `ROI_RESULT_KEYS`. Adding fields =
  forward-compatible (consumers ignore unknown keys); removing or
  renaming requires schema bump.

## Things that are deferred (do not silently implement)

- True per-pixel saliency-weighted pooling (Option A). Requires
  modifying libvmaf's `feature_collector.c`, much heavier ADR process
  — keep out of this Option C tool.

## When editing this directory

1. Run unit tests: `pytest tools/vmaf-roi-score/tests`.
2. Changing JSON schema -> bump `SCHEMA_VERSION`, update tests'
   canonical-key assertion, update `docs/usage/vmaf-roi-score.md`.
3. `--saliency-model` path supports little-endian planar 8/10/12/16-bit
   YUV (`yuv420p`, `yuv420p10le`, `yuv420p12le`, `yuv420p16le`, plus
   corresponding 4:2:2 / 4:4:4 variants). Big-endian high-bit-depth YUV
   unsupported. New pix_fmt family changes user-visible behaviour ->
   update `docs/usage/vmaf-roi-score.md`, add materialisation tests.
