---
paths:
  - tools/vmaf-tune/src/vmaftune/cache.py
  - tools/vmaf-tune/tests/test_cache.py
invariant: Cache key fields are load-bearing; cache content stays opaque and parsed through domain model.
---
<!-- markdownlint-disable MD024 -->
# Execution caching

- **Cache key fields are load-bearing
  ([ADR-0298](../../../docs/adr/0298-vmaf-tune-cache.md)).**
  `cache_key()` in `cache.py` digests `CACHE_VERSION` (2) and
  `src_sha256`, `encoder`, `preset`, `crf`, `adapter_version`,
  `ffmpeg_version`, `passes`, `sample_clip_seconds`,
  `sample_clip_start_s` and `settings` map, and refuses empty text
  fields. Dropping any one is silent correctness bug: stale entries
  shadow real results when adapter or ffmpeg is upgraded, or 1-pass
  result answers 2-pass request (corpus runner passed `""` for
  both versions until 2026-10-04). Contract is asserted by
  `test_cache_key_diffs_on_each_field` and corpus-level miss tests
  in `test_cache.py`. Every registered adapter declares non-empty
  `adapter_version` (`test_every_registered_adapter_declares_an_adapter_version`);
  bump it when adapter's argv shape, preset list, or quality range
  changes. Changing key's composition bumps `CACHE_VERSION`.
- **Cache content stays opaque.** Cache value is parsed
  `(bitrate, vmaf, encode_time, score_time)` tuple, miss row
  (`row`, stored as `[column, value]` pairs so column order
  survives sorted-key meta file; NaN as `null`) and opaque
  `<key>.bin` blob. Do not bake cache contents into JSONL row —
  row is canonical record, cache is sidecar. Cache hit must
  produce row that is bit-identical to cache miss (modulo
  `run_id`, `timestamp` and `encode_path`, which stays empty unless
  `--keep-encodes`); `test_corpus_hit_row_equals_miss_row` pins it.
