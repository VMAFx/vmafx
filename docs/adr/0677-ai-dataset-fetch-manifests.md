<!-- markdownlint-disable MD013 MD060 -->
# ADR-0677: AI Dataset Fetch Manifests

- **Status**: Accepted
- **Date**: 2026-05-21
- **Deciders**: Lusoris, Codex
- **Tags**: ai, datasets, provenance, training, fork-local

## Context

ADR-0661 made AI run provenance a shared schema. ADR-0668 through ADR-0676
then applied that schema to derived feature tables, corpus JSONL boundaries,
legacy trainer-input builders, source MOS adapters, and materializer batches.
Two older fetch helpers still sat before that evidence chain:

- `fetch_konvid_1k.py` downloads and extracts the KoNViD-1k videos/metadata
  archives used by early NR and learned-filter work.
- `fetch_youtube_ugc_subset.py` selects the smallest complete
  YouTube-UGC VP9 4-tuples and writes a content manifest used by the
  vmaf_tiny_v5 / UGC expansion experiments.

Both scripts create operator-local, gitignored inputs. Without a replay
manifest, later feature tables and model cards can cite a corpus root but not
the exact fetch selection, archive URLs, row cap, or output bundle that seeded
the run.

## Decision

Add deterministic run-manifest sidecars to both fetch helpers:

- `fetch_konvid_1k.py` writes `<root>/fetch_manifest.json` by default and
  accepts `--manifest-out PATH`.
- `fetch_youtube_ugc_subset.py` preserves the existing `--manifest` content
  manifest and adds `--run-manifest-out PATH`, defaulting to
  `<manifest>.run-manifest.json`.

Both sidecars reuse `aiutils.run_manifest.build_run_provenance()` and record
dataset identity, effective selection/download configuration, output paths,
and relevant counters. The YouTube helper keeps run metadata out of the
existing stem-to-files content manifest so existing consumers do not need a row
schema change.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add fetch-side run manifests | Closes the evidence gap before corpus conversion; reuses ADR-0661; no row-schema changes | Adds one more sidecar per fetch run | Chosen |
| Put run metadata into the YouTube content manifest | Single artifact | Breaks the simple `stem -> {orig,cbr,vod,vodlb}` shape existing callers expect | Rejected |
| Only document shell commands | No code change | Shell history is not durable evidence and cannot be cited by later model cards | Rejected |
| Defer until next full data refresh | Avoids a small PR now | The fetch gap would keep producing anonymous local inputs during the current AI refresh | Rejected |

## Consequences

- **Positive**: KoNViD and YouTube-UGC downloaded corpus roots can now be
  replayed or audited from a durable JSON sidecar before any extraction step.
- **Negative**: Operators will see an additional gitignored JSON file beside
  fetch outputs.
- **Neutral / follow-ups**: Regenerate local fetch manifests when refreshing
  KoNViD-1k or the YouTube-UGC subset before promoted model-card updates.

## References

- [ADR-0661](0661-ai-run-manifest-provenance.md) — shared AI run provenance.
- [ADR-0668](0668-ai-derived-table-provenance.md) — derived table manifests.
- [ADR-0676](0676-mos-corpus-adapter-manifests.md) — MOS corpus adapter
  manifests.
- Source: req — "so all, netflix, regressors, encoders etc... everything we did so far needs updates"
- Source: req — "well next backlog batch"
