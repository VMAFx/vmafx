<!-- markdownlint-disable MD060 -->
# `vmaf-tune` content-addressed cache

The `vmaf-tune` cache stores the encode and score result of every `corpus` cell
under a content hash, so a repeated `(source, encoder, preset, crf)` cell is
restored from disk instead of being encoded and scored again. It is a Python
API feature: there is no command-line flag, and it is **off by default**. The
design is in [ADR-0298](../adr/0298-vmaf-tune-cache.md); the tool overview is
in [`vmaf-tune.md`](vmaf-tune.md).

## Turn it on

The corpus runner enables the cache when both `cache_enabled` and `cache_dir`
are set on `CorpusOptions`:

```python
from pathlib import Path

from vmaftune.corpus import CorpusJob, CorpusOptions, iter_rows

opts = CorpusOptions(
    encoder="libx264",
    output=Path("corpus.jsonl"),
    cache_enabled=True,
    cache_dir=Path.home() / ".cache" / "vmaf-tune",
)
rows = list(iter_rows(job, opts))  # job: a CorpusJob for one source
```

| Option | Default | Meaning |
|---|---|---|
| `cache_enabled` | `False` | Master switch for the cache. |
| `cache_dir` | `None` | Cache root. The cache stays off while this is `None`, even with `cache_enabled=True`. |

`vmaf-tune corpus` has no `--cache-dir`, `--no-cache` or size flag. The only
`--cache-dir` on the command line belongs to the `sidecar` subcommand and is a
different cache.

`default_cache_dir()` in `tools/vmaf-tune/src/vmaftune/cache.py` returns
`$XDG_CACHE_HOME/vmaf-tune` (or `~/.cache/vmaf-tune` when the variable is
unset). The corpus runner does not call it, so pass that path yourself as
`cache_dir` if you want it.

## Cache key

A cache entry is keyed on the SHA-256 of the canonical-JSON encoding of
these fields plus the key version (`CACHE_VERSION`, 2):

| Field | Source |
|---|---|
| `src_sha256` | Content hash of the reference file. |
| `encoder` | Adapter slug (`libx264`, `hevc_nvenc`, ...). |
| `preset` | Encoder preset string fed to the adapter. |
| `crf` | Quality-knob value (int). |
| `adapter_version` | The adapter's `adapter_version`, bumped when its argv shape, presets or range change. |
| `ffmpeg_version` | The version `ffmpeg -version` reports, read once per sweep. |
| `passes` | `2` for a 2-pass encode (`two_pass` on an adapter that supports it), else `1`. |
| `sample_clip_seconds`, `sample_clip_start_s` | The encoded window of sample-clip mode. |
| `settings` | Everything else the encode or the score depends on: width, height, source width and height, pixel format, frame rate, duration, the extra encoder argv (HDR signalling, rung scale), the VMAF model and the score backend. |

`cache_key()` refuses an empty source hash, encoder, preset, adapter
version or ffmpeg version, and `tools/vmaf-tune/tests/test_cache.py`
checks that changing any field produces a new key. When `ffmpeg -version`
reports no version, the corpus runner keeps the cache off for that run
and logs a warning. Entries written under key version 1 hash differently,
so they miss; their files stay until LRU eviction or a manual delete.

## Cache layout on disk

```text
<cache_dir>/
  meta/<key>.json     parsed result tuple (size, times, vmaf score, versions)
  blobs/<key>.bin     opaque encoded artifact (atomic put: tmp + rename)
  __index__.json      last-access timestamps for LRU eviction
```

## Cache lifecycle

- **Hit:** the encode and score subprocesses are skipped and the row the miss
  produced is replayed, column for column, with a fresh `run_id` and
  timestamp. An entry without a stored row is treated as a miss.
- **Miss:** encode and score run as normal. A successful result (encoder exit
  status 0) is inserted with a fresh `last_access` timestamp.
- **Source hash off:** a run with `--no-source-hash` has no `src_sha256`, so
  the cache is skipped for every cell of that run.
- **LRU eviction:** each insert evicts least-recently-used entries until the
  total size of `meta/` plus `blobs/` is at or below the ceiling. The default
  ceiling is 10 GiB (`DEFAULT_SIZE_BYTES`). The corpus runner does not expose
  a size option; use `TuneCache(path, size_bytes=...)` directly to change it.
- **Index flush:** the runner writes `__index__.json` once, after the sweep.

## Caveats

- The cache is not baked into the JSONL row. The row stays the canonical
  record and the cache is an opaque result store, so rows look identical
  whether a cell hit or missed ([ADR-0298](../adr/0298-vmaf-tune-cache.md)).
- A hit does not write a synthetic `encode_path`; that field stays empty unless
  `--keep-encodes` is set.
- `TuneCache` is not thread-safe. Concurrent processes (for example over NFS)
  can share a directory: reads work, and writes are last-writer-wins with both
  writers' bytes valid by content addressing.

## Manual eviction

There is no cache prune subcommand. To start over, for example when the corpus
methodology changes incompatibly, delete the directory:

```shell
rm -rf ~/.cache/vmaf-tune
```

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — the tool overview.
- [`vmaf-tune-corpus.md`](vmaf-tune-corpus.md) — the `corpus` subcommand whose
  cells the cache stores.
- [`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md) — the adapters
  that `adapter_version` refers to.
- [`vmaf-tune-near-dup.md`](vmaf-tune-near-dup.md) — perceptual video
  fingerprinting for near-duplicate cache reuse.
- [ADR-0298](../adr/0298-vmaf-tune-cache.md) — design decision.
- [Research-0086](../research/0086-usage-doc-coverage-audit-2026-05-08.md) —
  audit that triggered this page.
