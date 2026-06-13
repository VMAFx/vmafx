# AI shared helpers extracted to aiutils package

Extract 5 common utility patterns from 18 `ai/scripts/` files into a reusable `ai/src/aiutils/` package:

1. **`file_utils.sha256(path) -> str`** — Streaming SHA-256 computation with 1 MiB buffering. Extracted from 14 identical call sites across all export and training scripts.

2. **`time_utils.now_iso_8601() -> str`** — Current UTC time as ISO-8601 second-precision string. Consolidates 2 time-fetching patterns from `aggregate_corpora.py` and `train_predictor_v2_realcorpus.py`.

3. **`jsonl_utils.iter_jsonl(path) -> Iterator[tuple[int, dict]]`** — JSONL line-by-line iteration with blank-line skipping and error reporting. Extracted from 2 corpus-aggregation sites.

4. **`parquet_utils.write_parquet_atomic(df, output, **kwargs) -> None`** — Atomic Parquet writes using temp-file + rename. Extracted from `enrich_k150k_parquet_metadata.py`.

Updated scripts import from `aiutils` instead of defining helpers locally. Net savings: ~160 LOC deleted (from 16 scripts), ~90 LOC added (aiutils modules + imports). See `ai/src/aiutils/AGENTS.md` for invariants for future scripts.
