# aiutils — Shared AI Helpers

Parent: [../../AGENTS.md](../../AGENTS.md) (ai/).

Central utility patterns. Cuts duplication across `ai/scripts/` and
`tools/vmaf-tune/src/vmaftune/` (and downstream adding `ai/src` to `sys.path`).

## Invariants for new scripts and modules

Rules for new script in `ai/scripts/` or module in
`tools/vmaf-tune/src/vmaftune/`:

1. **File hashing:** Import `sha256` from `aiutils.file_utils`, not local
   `_sha256()`.
2. **UTC timestamps:** Use `now_iso_8601()` from `aiutils.time_utils` for
   ISO-8601 second-precision UTC (not ad hoc `.isoformat()` calls).
3. **JSONL iteration:** Use `iter_jsonl()` from `aiutils.jsonl_utils` for
   newline-delimited JSON, not inline generators.
4. **Atomic Parquet writes:** Use `write_parquet_atomic()` from
   `aiutils.parquet_utils` for safe DataFrame write + cleanup on failure.
   ADR-0926: helper default = **schema v2** (zstd-3 compression, canonical
   column order: `clip_id`, `frame_idx`, sorted features, labels, metadata;
   pyarrow file metadata with `vmafx_schema_version` + `vmafx_pipeline_hash`).
   Pass `compression="snappy"` if downstream cannot read zstd. Pass explicit
   `labels=` / `metadata=` to override column heuristics. Detection:
   `read_parquet_with_schema(path)` -> `(df, schema_version)` (v1 = legacy raw
   `df.to_parquet(...)`, v2 = helper). **Do not** call `df.to_parquet(...)`
   directly in new code.
5. **Run provenance:** Use `aiutils.run_manifest.write_run_manifest()` for
   script sidecars needing stable entrypoint, args, input, output metadata,
   adapter counts/config. Use `build_run_provenance()` only when embedding
   provenance block into existing report schema. Do not hand-roll path hashing
   or manifest envelope in each script.
6. **CLI setup:** Use `make_argument_parser()` and `collect_cli_argv()` from
   `aiutils.cli_helpers` for operator scripts. Batch manifest runners must also
   use `add_batch_manifest_arguments()`: keeps `--manifest`, `--base-dir`,
   `--report-json`, `--report-md`, `--fail-fast`, optional
   `--allow-row-failures` aligned.
7. **Direct script bootstrap:** `aiutils` must stay importable without mutating
   `sys.path`. Direct `ai/scripts/*.py` entrypoints use
   `ai/scripts/_script_bootstrap.py` before importing package; do not move
   bootstrap into `aiutils` (too late to solve import).

## Module inventory

- `file_utils.py` — `sha256(path) -> str`
- `time_utils.py` — `now_iso_8601() -> str`
- `jsonl_utils.py` — `iter_jsonl(path) -> Iterator[tuple[int, dict]]`
- `parquet_utils.py` — `write_parquet_atomic(df, output, **kwargs) -> None`,
  `read_parquet_with_schema(path) -> (df, int)`,
  `detect_schema_version(path) -> int`,
  `apply_standard_column_order(df, *, labels=None, metadata=None) -> DataFrame`
  (ADR-0926; schema v2 = on-disk default)
- `run_manifest.py` — deterministic `run_provenance` sidecar helpers
- `cli_helpers.py` — shared parser/raw-argv/batch-manifest argument helpers
