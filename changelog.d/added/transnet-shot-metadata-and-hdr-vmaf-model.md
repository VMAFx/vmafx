- **TransNet-V2 shot-metadata corpus columns + HDR VMAF model port slot
  ([Research-0086](../docs/research/0086-contributor-pack-web-data-expansion-2026-05-08.md),
  [ADR-0300 § Status update 2026-05-08](../docs/adr/0300-vmaf-tune-hdr-aware.md)).**
  Two related fork-internal additions to the `vmaf-tune` corpus
  generator:
  1. `iter_rows` now runs `vmaf-perShot` (TransNet-V2 wrapper, ADR-0223)
     once per source and emits three additive columns: `shot_count`,
     `shot_avg_duration_sec`, `shot_duration_std_sec`. The std column
     gives Phase B / C predictors a free content-class proxy
     (animation vs. live-action) at zero additional encode cost.
     Sources where TransNet is unavailable get `(0, 0.0, 0.0)` so
     downstream loaders can filter on `shot_count > 0`. The schema
     stays at v2 — keys are purely additive.
  2. `select_hdr_vmaf_model()` learns transfer-aware routing
     (`transfer="pq"`/`"hlg"`) and prefers the canonical Netflix
     filename `vmaf_hdr_v0.6.1.json` over the legacy glob. A new
     `hdr_model_name_for(transfer)` helper exposes the dispatch table.
     The actual model JSON is **not** shipped — Netflix publishes it
     outside their public `model/` tree (re-verified against
     `upstream/master` 2026-05-08) and a fork-local license review is
     a follow-up. The slot warns once-per-process on miss; dropping
     a licensed copy at `model/vmaf_hdr_v0.6.1.json` requires no
     further code change. (lusoris fork only)
