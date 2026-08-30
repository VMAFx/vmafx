Added a CHUG-specific HDR MOS-head training entry point that consumes
reference-aligned CHUG feature JSONL shards and emits a local
`chug_hdr_mos_head_v1` manifest instead of routing HDR MOS experiments
through KonViD-named CLI flags.

The CHUG wrapper now defaults to the `chug-hdr-wide-v1` feature schema,
which adds temporal p10/p90/std aggregates and HDR ladder metadata while
keeping the committed KonViD `konvid-v1` 11-column schema intact.
