# ADR-0993: KoNViD / UGC / BVI-DVC Saliency Batch Manifests and Run Scaffolding

- **Status**: Accepted
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: ai, saliency, materializer, konvid, ugc, bvi-dvc, batch, fork-local

## Context

[ADR-0673](0673-saliency-materializer-batch-manifest.md) introduced
`ai/scripts/batch_materialize_saliency_features.py` and the batch manifest
schema. [PR #540](https://github.com/VMAFx/vmafx/pull/540) fixed the Netflix
saliency materializer (raw YUV flags, per-file cache, path-column diagnostic)
and confirmed 11,190 Netflix rows at 0 failures. The CHUG canonical-shards
run also completed successfully (5,136 rows).

The remaining corpora — KoNViD-150K, YouTube UGC, and BVI-DVC — each carry a
different path-resolution challenge:

- **KoNViD-150K** (`konvid_150k.jsonl`): has `src` (relative clip filename),
  `width`, and `height` columns. Clips reside at
  `.corpus/konvid-150k/k150ka_extracted/`. This corpus is directly materialisable
  using the existing batch runner with no preprocessing step.

- **YouTube UGC** (full-feature parquet `full_features_ugc_refresh_20260520.parquet`):
  the `source` column holds corpus identifiers such as
  `ugc-Gaming_1080P-223e-cbr`, not file paths. The actual files are at
  `.corpus/ugc/download/{ContentName}_{Variant}.{webm,mp4}`. A path-enriched
  corpus JSONL must be generated first (via `youtube_ugc_to_corpus_jsonl.py`
  or a thin path-derivation step).

- **BVI-DVC** (full-feature parquet `full_features_bvi_dvc_D_refresh_20260520.parquet`):
  the `key` column encodes encode parameters (e.g.
  `DAdvertisingMassagesBangkokVidevo_480x272_25fps_10bit_420`), not file paths.
  Raw reference YUVs live at `.corpus/bvi-dvc-raw/`. A path-enriched JSONL
  or a direct `--root` pass through the raw YUV directory requires mapping
  key back to the source YUV filename.

## Decision

Commit in-tree batch manifests under `ai/batch-manifests/saliency/`:

- `konvid-150k.json` — fully wired; resolves `konvid_150k.jsonl` → clips at
  `.corpus/konvid-150k/k150ka_extracted/`. Ready to run.
- `ugc.json` — scaffolded with `tables: []` and `_status` / `_resolution`
  comments that document the blocking gap and both resolution options.
- `bvi-dvc.json` — scaffolded with `tables: []` and `_status` / `_resolution`
  comments that document the blocking gap, the raw YUV resolution, and the
  required `-f rawvideo` flags (already handled by the materializer when
  extension is `.yuv` and `default_width`/`default_height` are set).

This makes the three corpus manifests visible to agents and reviewers, gives
the exact run command for KoNViD-150K, and records the UGC / BVI-DVC gaps
as in-tree scaffolding stubs rather than undocumented state in `.workingdir2/`.

The KoNViD-150K smoke-run command (10-row limit) is documented in the PR
description. The full run touches 148,543 clips and should be scheduled as a
long-running background job.

## Alternatives Considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Commit manifests only after all three are wired | Fully runnable from day one | Blocks the PR on UGC/BVI path-enrichment work; KoNViD-150K can run now | Rejected: KoNViD-150K is already unblocked and the UGC/BVI gaps are worth recording in-tree |
| Keep manifests in `.workingdir2/` only | Zero tree churn | Gitignored; lost on clone; invisible to agents parsing the codebase | Rejected: the existing CHUG/Netflix manifests in `.workingdir2/` are already unreachable to fresh clones |
| Generate UGC/BVI path-enriched JSONL in this PR | Both blocked corpora become runnable | Significantly expands scope; UGC download dir may not be present on all machines | Deferred: add as a follow-on PR once the path-derivation strategy is decided |

## Consequences

- **Positive**: KoNViD-150K saliency can be launched from a versioned,
  in-tree manifest; UGC and BVI-DVC gaps are documented alongside the runnable
  manifest so the next agent can unblock them in one PR.
- **Negative**: Two manifests ship with empty `tables` arrays; running them
  is a no-op until a follow-on populates the tables.
- **Neutral / follow-ups**:
  - UGC follow-on: generate `ugc_corpus.jsonl` via `youtube_ugc_to_corpus_jsonl.py`
    and populate `ugc.json`.
  - BVI-DVC follow-on: generate a raw-YUV corpus JSONL mapping key back to
    source filename and populate `bvi-dvc.json`.
  - After all three corpora have saliency columns, re-run the signal-mix audit
    (`ai/scripts/signal_mix_audit.py`) and measure predictor / MOS-head impact.

## References

- [ADR-0655](0655-saliency-feature-materializer.md) — shared saliency table materializer.
- [ADR-0673](0673-saliency-materializer-batch-manifest.md) — batch manifest schema.
- [ADR-0672](0672-saliency-materializer-temporal-controls.md) — model and
  temporal-reducer metadata.
- [ADR-0661](0661-ai-run-manifest-provenance.md) — AI run provenance schema.
- [PR #540](https://github.com/VMAFx/vmafx/pull/540) — Netflix path-column and
  raw YUV fix.
- Source: req — "KoNViD / UGC / BVI saliency materializer runs — Manifests
  merged but 0 actual runs executed"
