<!-- markdownlint-disable MD060 -->
# Research-0073: vmaf-tune content-addressed cache

- **Status**: Adopted by [ADR-0298](../adr/0298-vmaf-tune-cache.md)
- **Date**: 2026-05-03

## Question

Iterative `vmaf-tune corpus` runs spend most of their wall clock
re-encoding and re-scoring `(src, encoder, preset, crf)` tuples that
have not changed between invocations. What is the smallest possible
addition that makes re-runs free without compromising correctness
(stale results) or row schema (Phase B/C consumers)?

## Option space

We considered six points along two axes — *what to key on* and
*where to store* — and one orthogonal axis (*default ON vs OFF*).

### Axis 1 — key composition

Three candidates:

1. **Content-hash six-tuple (chosen)**: `sha256(src_content) +
   encoder + preset + crf + adapter_version + ffmpeg_version`. Stable
   under file-rename, file-mtime drift, and shell-history rewrites.
   Cost: one full-file SHA-256 read per source per run (already
   computed today by the corpus row builder).
2. **Path + mtime six-tuple**: cheap (no full-file hash). Fails on
   re-recorded references with the same path; fails on archive →
   working-copy round-trips that bump mtime without changing
   content.
3. **Just `(encoder, preset, crf)`**: tiny key space. Fails the
   moment a user runs the same sweep against a different reference.
   Trivially wrong.

The user's hard rules (do not skip version components) forced (1).

### Axis 2 — storage

1. **Local on-disk under XDG (chosen)**: zero deps, zero daemon,
   atomic-rename writes are NFS-safe so a shared mount works for
   distributed cache without additional code.
2. **In-memory only**: defeated by the cross-invocation use case.
3. **S3 / object store**: pulls in a runtime dep (`boto3` or
   similar) and complicates the trivial-on-laptop case. Deferred —
   the chosen layout is forward-compatible with a remote backend
   slotted in behind the same `TuneCache` API.

### Axis 3 — default

1. **ON (chosen)**: re-runs are the dominant interaction; the
   default has to optimise for the dominant interaction. Worst-case
   for first-time users: cache directory under XDG appears with
   ≤10 GiB of blobs.
2. **OFF**: needs every user to learn the flag exists before they
   benefit. Hostile to the iterative loop.

## Wall-clock numbers (informal)

Single-clip 6-cell `(medium, slow) × (22, 28, 34)` sweep against a
1080p10s YUV on a Ryzen 7 7700 + RTX 4070:

| Path | Wall-clock |
|---|---|
| Cold run (cache empty) | ~12 minutes |
| Re-run, all hits | ~250 ms (probe + 6 fs-read) |
| Re-run, one cell changed | ~2 minutes (1 encode + 1 score) |

The "one cell changed" case is the win — re-running after a flag
adjustment now scales with the *number of changed cells*, not the
grid size.

## Risks

- **Disk pressure**: a sweep producing >10 GiB of unique encodes
  evicts older entries. Mitigated by the `--cache-size-gb` flag for
  users who deliberately want a larger cache (16-bit HDR sweeps,
  long durations).
- **Corruption-as-miss**: a half-written blob caused by a SIGKILL
  mid-`put` will read as a miss (atomic rename means either the
  full blob or nothing — no torn writes — but a separately
  truncated meta JSON would cause `get` to return None). Acceptable
  for now; a future `cache verify` subcommand would close the loop.
- **Adapter-version drift**: relies on per-adapter manual bumps.
  The first time someone forgets, stale entries return on the new
  adapter shape. Mitigated by the test that asserts each key field
  flips the digest — but only catches it once a test exists.

## Outcome

Implemented as ADR-0298. See `tools/vmaf-tune/src/vmaftune/cache.py`
for the module and `tools/vmaf-tune/tests/test_cache.py` for the
contract suite (12 tests covering key stability, eviction, and
end-to-end miss-then-hit).
