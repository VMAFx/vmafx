# vmaf-tune cache index batching

vmaf-tune's `TuneCache` now defers `__index__.json` writes via a dirty-flag mechanism instead of writing on every cache `get` (hit) and `put` (miss). Index writes are now batched and flushed only at the end of a sweep via an explicit `flush()` call in `iter_rows`, or during LRU eviction.

**Motivation:** On a 1000-cell cold sweep with no cache hits, the old behavior rewrote the 4 kB index file 1000 times (once per `put`). The new behavior writes once at the end, eliminating ~500 ms of redundant I/O.

**Behavior change:** The cache index is kept in memory and marked dirty on every access. To prevent data loss on ungraceful exit, `iter_rows` now calls `cache.flush()` after all rows have been yielded. External code that directly instantiates `TuneCache` and performs puts should call `flush()` before exiting.

**Related:** ADR-0298 (cache architecture), perf-audit-pipeline-2026-05-16 (win #4).
