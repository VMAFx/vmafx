# ADR-0371 — Shared `CorpusIngestBase` for MOS-corpus ingestion adapters

| Field       | Value                                |
|-------------|--------------------------------------|
| Status      | Accepted                             |
| Date        | 2026-05-10                           |
| Scope       | ai, corpus, refactor, fork-local     |
| PR          | (this PR)                            |

## Context

Six MOS-corpus ingestion scripts (KonViD-1k, KonViD-150k, LSVQ,
LIVE-VQC, Waterloo IVC 4K-VQA, YouTube UGC) each duplicated
approximately 200 lines of identical boilerplate:

- `_sha256_file` — chunked SHA-256 with 1 MiB reads
- `_utc_now_iso` — second-precision ISO-8601 UTC timestamp
- `probe_geometry` — ffprobe JSON geometry extractor with identical
  command construction, stream selection, and error handling
- `_pick` — case-insensitive CSV column picker (identical across all
  six files)
- `_parse_framerate` — rational `a/b` ffprobe framerate parser
- `load_progress` / `save_progress` / `mark_done` / `mark_failed` /
  `should_attempt` — resumable-download progress state (four scripts)
- `download_clip` — curl-backed per-clip downloader (four scripts)
- `_read_existing_sha_index` — JSONL dedup reader
- `RunStats` — aggregate counters class (four scripts)
- `build_row` — JSONL row assembler (identical output schema across all)

Total duplicated lines: ~1 200 across the six files (average ~200 per
script), in addition to ~1 600 lines of corpus-specific CSV parsing and
CLI that are legitimately different.

The user requested consolidation to reduce maintenance surface and
prevent future divergence as new corpora are added.

## Decision

Extract the shared boilerplate into `ai/src/corpus/base.py` as a
package-level module exporting:

- Free functions: `sha256_file`, `utc_now_iso`, `pick`,
  `normalise_clip_name`, `probe_geometry`, `load_progress`,
  `save_progress`, `mark_done`, `mark_failed`, `should_attempt`,
  `download_clip`, `read_sha_index`
- `RunStats` class (aggregate counters)
- `CorpusIngestBase` ABC with:
  - `__init__` accepting all shared configuration (corpus_dir, output,
    ffprobe_bin, curl_bin, corpus_version, runner seam, etc.)
  - Abstract `iter_source_rows(clips_dir)` that subclasses override
  - `run()` orchestrator implementing the probe-SHA-write-dedup loop

Each of the six scripts is refactored to a `~80-150 LOC` file
containing only:

- Corpus-specific constants and CSV column aliases
- A `parse_manifest_csv` function (the only legitimately different
  logic per script)
- A `CorpusIngestBase` subclass with `iter_source_rows` implemented
- A backward-compatible module-level `run()` function preserving the
  existing test and caller API
- The `argparse` CLI

`bvi_dvc_to_corpus_jsonl.py` is intentionally excluded: it wraps the
`vmaftune.CORPUS_ROW_KEYS` schema (a full-reference encode-quality
corpus), not the MOS-ingest schema; conflating the two would couple
unrelated abstractions.

## Consequences

### Positive

- Single authoritative implementation of ffprobe invocation, SHA-256
  dedup, resumable-download state management, and the JSONL row schema
  builder — future bug fixes propagate to all corpora automatically.
- New corpora require only a `parse_manifest_csv` function and a
  ~10-line `CorpusIngestBase` subclass.
- Test coverage of shared logic lives in one place
  (`ai/tests/test_corpus_base.py`) rather than being re-tested across
  six per-corpus test files.

### Negative / risks

- `from corpus.base import ...` requires `PYTHONPATH=ai/src` (already
  the convention in this repo's `conftest.py` and CI).
- The `CorpusIngestBase.run()` materialises `iter_source_rows` into a
  list before the main loop (to support `max_rows` capping). Very large
  manifests (KonViD-150k ~150 000 rows) will hold the full parsed list
  in memory simultaneously with the JSONL output handle. The rows are
  pure-Python dicts (~200 bytes each); 150 000 rows is ~30 MB — well
  within acceptable limits.

## Alternatives considered

**Keep the duplication, enforce via linting.** A `semgrep` rule could
detect divergence between the six probe_geometry implementations. Ruled
out: linting catches drift after the fact; the base-class approach
prevents it structurally.

**Use a code-generation approach (Jinja templates).** Ruled out:
adds a build step and loses static analysis on generated code.
The object-oriented base-class approach is directly readable and
importable.

## References

- req: "Extract a shared CorpusIngestBase class to ai/src/corpus/base.py
  for the 7 corpus ingestion scripts. Each script duplicates ~200 lines
  of ffprobe / SHA-index / JSONL-append / argparse boilerplate."
- ADR-0325 (KonViD corpus ingestion)
- ADR-0333 / ADR-0367 (LSVQ)
- ADR-0368 (YouTube UGC)
- ADR-0369 (Waterloo IVC 4K-VQA)
- ADR-0370 (LIVE-VQC)
