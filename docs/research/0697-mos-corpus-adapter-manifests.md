# Research-0697: MOS Corpus Adapter Manifests

## Question

Which remaining MOS-corpus source adapters still produce durable JSONL shards
without replayable input/output evidence, and what is the smallest contract that
makes those shards usable as model-card evidence?

## Findings

The downstream AI pipeline already had provenance sidecars for training reports,
derived feature tables, aggregate/merge JSONL outputs, and several legacy
trainer-input builders. The source MOS adapters were the weak boundary:
CHUG, KoNViD, YouTube-UGC, LSVQ, LIVE-VQC, and Waterloo-IVC rows could be
generated from local dataset roots, manifest CSVs, row caps, and resumable
progress files, but the emitted JSONL did not preserve those run-level choices.

That matters more than normal file metadata. The same output path can represent
a capped smoke subset, a full-corpus run, or a rerun after download attrition
changed. Later MOS-head training, signal-mix audits, and model cards need the
adapter-level counters before aggregation normalises scales or deduplicates
rows.

## Decision Drivers

- Keep row schemas stable; run-level evidence belongs beside the JSONL, not in
  every row.
- Reuse ADR-0661 `run_provenance` so corpus source sidecars look like the rest
  of the AI evidence chain.
- Make the sidecar automatic with `<output>.manifest.json`, but expose
  `--manifest-out` for dated experiment bundles.
- Cover the full source-adapter family in one batch so mixed-corpus training
  does not have partial provenance.

## Implementation Notes

`ai/src/corpus/base.py` now owns `write_ingest_manifest()`. Adapters pass their
effective input paths, output paths, row caps, corpus version, and run counters
into that helper after a successful JSONL run. Tests cover the shared helper and
the CHUG CLI default sidecar; existing adapter tests continue to exercise row
compatibility.

## Follow-Up

Regenerate local CHUG, KoNViD, YouTube-UGC, LSVQ, LIVE-VQC, and Waterloo-IVC
JSONL shards before using them as promoted MOS-head training evidence, and keep
the generated sidecars with the gitignored corpus outputs.
