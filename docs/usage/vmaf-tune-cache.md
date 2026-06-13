# `vmaf-tune` content-addressed cache

`vmaf-tune` corpus runs are iterative — most user sessions re-run the
same `(source, encoder, preset, crf)` tuples after adjusting one
unrelated flag. Re-encoding and re-scoring those unchanged tuples
burns minutes-to-hours of wall-clock time for no new information.
Per [ADR-0298](../adr/0298-vmaf-tune-cache.md) the corpus runner
ships a content-addressed cache that turns those repeat cells into
free hits.

The cache is **on by default** for new runs. `--no-cache` disables
it. The base tool is documented in [`vmaf-tune.md`](vmaf-tune.md).

## Cache key

A cache entry is keyed on the SHA-256 of the canonical-JSON-encoded
tuple of *six* fields, all of which must match for a hit:

| Field             | Source                                       |
|-------------------|----------------------------------------------|
| `src_sha256`      | Content hash of the reference YUV.           |
| `encoder`         | Adapter slug (`libx264`, `hevc_nvenc`, …).   |
| `preset`          | Encoder preset string fed to the adapter.    |
| `crf`             | Quality-knob value (int).                    |
| `adapter_version` | Bumps when the adapter's argv shape changes. |
| `ffmpeg_version`  | Host ffmpeg version string.                  |

Dropping any one of these fields would produce a wrong cached score
(e.g. on an adapter upgrade or an ffmpeg rebuild). Each field
participates in the key by design; tests in
`tools/vmaf-tune/tests/test_cache.py` enforce that mutating any one
field produces a new key.

`src_sha256` can be skipped with `--no-source-hash` for fast iteration
on huge YUVs at the cost of provenance — the cache will then key
purely on path identity.

## Cache layout on disk

```text
$XDG_CACHE_HOME/vmaf-tune/             (default; `~/.cache/vmaf-tune/` if XDG unset)
  meta/<key>.json                      JSON sidecar with the parsed result tuple
  blobs/<key>.bin                      opaque encoded artifact (atomic put: tmp + rename)
  __index__.json                       last-access timestamps for LRU eviction
```

## CLI shape

```shell
vmaf-tune corpus \
    --source ref.yuv --width 1920 --height 1080 \
    --pix-fmt yuv420p --framerate 24 \
    --encoder libx264 --preset medium --crf 22 \
    --out corpus.jsonl
# → cache enabled, default dir
```

| Flag                     | Effect                                                   |
|--------------------------|----------------------------------------------------------|
| (default)                | Cache enabled at `$XDG_CACHE_HOME/vmaf-tune/`.           |
| `--no-cache`             | Disable cache for this run; always re-encode + re-score. |
| `--cache-dir <path>`     | Override cache root.                                     |
| `--cache-size-bytes <N>` | Override LRU ceiling (default 10 GiB).                   |

The CorpusOptions library API exposes `cache_enabled` /
`cache_dir` / `cache_size_bytes` for in-process callers.

## Cache lifecycle

- **Hit**: the encode + score subprocesses are skipped entirely; the
  cached result tuple is reused as the JSONL row.
- **Miss**: encode + score run as normal; the result tuple is
  inserted with a fresh `last_access` timestamp.
- **LRU eviction**: when the on-disk cache size exceeds
  `--cache-size-bytes`, the LRU `__index__.json` drops least-recently-
  accessed entries until under the ceiling. The eviction step is
  invoked at the end of each `corpus` run, not opportunistically
  during it (so a single run cannot evict its own entries).

Per ADR-0298 **the cache content is not baked into the JSONL row** —
the JSONL row remains the canonical record. The cache is an opaque
encode/score result store keyed by the six-tuple; corpus rows on
disk look identical whether the cache hit or missed.

## Manual eviction

```shell
rm -rf ~/.cache/vmaf-tune       # nuke everything
```

There is no "cache prune" subcommand yet — manual `rm -rf` is the
intended escape hatch when the corpus methodology changes
incompatibly.

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — the base tool, corpus + recommend
  flow.
- [`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md) — the
  `adapter_version` cache-key field bumps any time the listed
  adapter shape changes.
- [ADR-0298](../adr/0298-vmaf-tune-cache.md) — design decision.
- [Research-0086](../research/0086-usage-doc-coverage-audit-2026-05-08.md)
  — audit that triggered this page.
