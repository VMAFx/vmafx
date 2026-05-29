# ADR-0838: CHUG Feature Extraction Replay Manifest Sidecar

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: Lusoris maintainers
- **Tags**: ai, training, provenance, chug

## Context

`ai/scripts/chug_extract_features.py` materialises full-reference VMAF feature
rows from local CHUG clips and writes them to a JSONL output file. Before this
ADR the script called `build_run_provenance()` but only embedded the provenance
block inside the optional `--split-manifest` and `--audit-output` sidecars.
When those flags were omitted (the common case in batch runs), the feature JSONL
was an anonymous artifact with no record of the input JSONL, clips directory,
feature set, vmaf binary, or command-line arguments that produced it.

ADR-0668 requires that operator-facing scripts creating derived feature tables
emit a `<out>.manifest.json` sidecar by default. CHUG feature extraction was
the remaining gap in that rule's coverage.

## Decision

`chug_extract_features.py` now writes a `<output>.manifest.json` sidecar after
each extraction run using `aiutils.run_manifest.write_run_manifest()`:

- Schema key: `chug-feature-extraction-manifest-v1`
- Fields: `run_provenance` (inputs, outputs, argv, args), `written_rows`,
  `feature_set`
- Default path: `<output>.manifest.json` (sibling of the feature JSONL)
- Override: `--manifest-out <path>`

The `--split-manifest` and `--audit-output` sidecars retain their existing
optional behaviour but no longer need to carry `run_provenance` to serve as the
sole provenance record; the top-level manifest is authoritative.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Top-level `<output>.manifest.json` (chosen) | Matches ADR-0668/ADR-0670 contract; single authoritative sidecar; zero parquet/JSONL schema churn | One extra JSON file per run | Chosen |
| Embed provenance in split-manifest only | No new file | Split-manifest is opt-in; most runs omit it | Rejected — leaves the common path anonymous |
| No change; defer to shell history | Zero code change | Unreplayable artifact; model cards cannot cite source | Rejected |

## Consequences

- Extraction runs now always produce a companion `*.manifest.json` sidecar.
- Downstream model cards and trainer manifests can reference the sidecar to
  prove which CHUG clips, feature set, and binary were used.
- The split-manifest and audit-JSON sub-sidecars continue to work as before;
  they no longer embed `run_provenance` when called from `main()` (they still
  accept it as a parameter for programmatic callers).

## References

- [ADR-0668](0668-ai-derived-table-provenance.md) — derived table manifests
- [ADR-0670](0670-ai-legacy-corpus-extraction-manifests.md) — legacy corpus extraction manifests
- [ADR-0661](0661-ai-run-manifest-provenance.md) — shared run provenance schema
- paraphrased: per user direction, audit CHUG extraction status and apply small fixes
