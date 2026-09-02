# tools/vmaf-roi-score — agent notes

Parent: [../../AGENTS.md](../../AGENTS.md).

## What this directory is

Option C implementation for region-of-interest VMAF *scoring* — drives
the `vmaf` CLI twice (full-frame + saliency-masked) and blends the
pooled scores. Pure Python; no libvmaf C-side changes. See
[ADR-0296](../../docs/adr/0296-vmaf-roi-saliency-weighted.md) and
[ADR-0424](../../docs/adr/0424-vmaf-tune-corpus-benchmark.md).

> **Naming guard**: do **not** rename this tool to `vmaf-roi`. That
> name belongs to `core/tools/vmaf_roi.c` (ADR-0247), the
> encoder-steering sibling that emits per-CTU QP-offset sidecars.
> Different surface, different output, related model. Confusing the
> two would silently break downstream encoder pipelines.

## Rebase-sensitive invariants

- **None.** `tools/vmaf-roi-score/` is wholly fork-local. There is no
  upstream Netflix/vmaf surface that owns or interacts with this
  directory; an upstream sync cannot conflict here.
- The combine math (`blend_scores`) is a pure linear blend on Python
  `float`. Tests pin the endpoints (`w=0` / `w=1`) and the midpoint.
  Changing the math is a schema-version bump (`SCHEMA_VERSION` in
  `src/vmafroiscore/__init__.py`) and an ADR-0296/0424 supersession.
- The JSON output schema is pinned by `ROI_RESULT_KEYS`. Adding fields
  is forward-compatible (consumers should ignore unknown keys);
  removing or renaming requires a schema bump.

## Things that are deferred (do not silently implement)

- True per-pixel saliency-weighted pooling (Option A). That requires
  modifying libvmaf's `feature_collector.c` and is a much heavier ADR
  process — keep it out of this Option C tool.

## When editing this directory

1. Run the unit tests: `pytest tools/vmaf-roi-score/tests`.
2. If you change the JSON schema, bump `SCHEMA_VERSION`, update the
   tests' canonical-key assertion, and update
   `docs/usage/vmaf-roi-score.md`.
3. The `--saliency-model` path supports little-endian planar 8/10/12/16-bit
   YUV (`yuv420p`, `yuv420p10le`, `yuv420p12le`, `yuv420p16le`, and the
   corresponding 4:2:2 / 4:4:4 variants). Big-endian high-bit-depth YUV
   remains unsupported. Any new pix_fmt family changes user-visible behaviour;
   update `docs/usage/vmaf-roi-score.md` and add materialisation tests.
