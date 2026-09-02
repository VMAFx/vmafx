<!-- markdownlint-disable MD060 -->
# ADR-0298: vmaf-tune content-addressed encode/score cache

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: lusoris, Claude
- **Tags**: `tools`, `vmaf-tune`, `cache`, `fork-local`

## Context

`vmaf-tune` (ADR-0237) drives a `(preset, crf)` grid through ffmpeg
and the libvmaf CLI for every reference clip in the sweep. A typical
iterative workflow looks like:

1. User runs the corpus sweep over a 6-cell grid against three clips
   to validate a code change. The sweep takes ~12 minutes wall-clock
   on a workstation (encode + score are both real subprocess work).
2. User notices a flag was wrong, adjusts it, re-runs the same
   sweep. Every cell re-encodes and re-scores even though the
   `(src, encoder, preset, crf)` tuples are byte-identical to the
   previous run.

The user's perceptual cost is "I changed one flag, why did this take
12 minutes again?" The encoder + scorer outputs are deterministic
functions of `(src_content, encoder, preset, crf, adapter_argv_shape,
ffmpeg_version)` — there is nothing in that tuple that the user
asked to change between runs. A content-addressed cache that
short-circuits both subprocess calls on a hit collapses re-runs from
minutes to milliseconds.

## Decision

We ship a content-addressed cache as `vmaftune.cache` with these
properties:

1. **Key**: SHA-256 of the canonical-JSON-encoded six-tuple
   `(src_sha256, encoder, preset, crf, adapter_version,
   ffmpeg_version)`. All six are mandatory; missing any one would
   let stale entries shadow real results when the adapter or ffmpeg
   is upgraded.
2. **Value**: a small JSON sidecar with the parsed
   `(encode_size_bytes, encode_time_ms, encoder_version,
   ffmpeg_version, vmaf_score, vmaf_model, score_time_ms,
   vmaf_binary_version)` tuple, plus an opaque `<key>.bin` artifact
   blob next to it.
3. **Location**: `$XDG_CACHE_HOME/vmaf-tune/` with the documented
   `~/.cache/vmaf-tune/` fallback. Override via `--cache-dir`. Never
   writes outside the configured cache root.
4. **Eviction**: LRU with a default 10 GiB ceiling
   (`--cache-size-gb`). The on-disk `__index__.json` carries
   last-access timestamps; on every `put`, oldest entries are
   dropped until total size ≤ cap.
5. **Default**: ON. Re-runs are the dominant interaction; the
   default has to optimise for the dominant interaction. `--no-cache`
   opts out for the rare re-validation case.
6. **Row contract**: the JSONL corpus row stays the canonical
   record. The cache is an opaque sidecar — its contents are
   never baked into the row, and a cache hit produces a row that
   is bit-identical (modulo `encode_path`, which stays empty
   unless `--keep-encodes`) to a cache miss.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Content-addressed local cache (chosen)** | Re-runs collapse to ms; key is content-stable; no daemon, no service. | Disk pressure if sweeps run on >100 GB of YUVs (mitigated by 10 GiB LRU cap). | Selected — matches the dominant workflow (iterative re-runs after a flag tweak). |
| Path-keyed cache (filename instead of `src_sha256`) | Cheaper key (no full-file hash). | Two YUVs with the same path but different content (overwritten ref) silently return wrong cached scores. | Rejected — silent correctness bug for a tiny perf win on the warm path. |
| Cache only the parsed tuple, not the artifact | Smaller on disk; no blob copy. | A future Phase B that wants to re-score with a different VMAF model can't, because the encode is gone. | Rejected — the artifact blob is the load-bearing asset; without it the cache is useless to any downstream consumer that varies a non-encode-affecting knob. |
| In-memory only (lru_cache decorator) | Trivial. | Doesn't survive across `vmaf-tune` invocations — defeats the purpose, since the user re-launches the CLI between flag tweaks. | Rejected — the use case is cross-process. |
| Distributed-only (NFS / S3) | Team-shareable. | Pulls in optional deps and a daemon. | Deferred — the on-disk layout is NFS-safe (atomic rename writes), so a shared mount works today via `--cache-dir`. A first-class S3 backend is a separate ADR if it ever materialises. |
| Skip `ffmpeg_version` in the key | Simpler key derivation (no probe call). | Re-running after `apt upgrade ffmpeg` returns scores from the old binary. | Rejected — explicitly listed as a hard rule by the requesting user; correctness over key brevity. |
| Skip `adapter_version` in the key | Simpler adapter contract. | Bumping the adapter (e.g. preset list change in a future PR) returns stale scores. | Rejected — same correctness argument; cheap to surface as a string field on the adapter. |

## Consequences

- **Positive**: re-runs of the same sweep are effectively free
  (single ffmpeg-version probe + N filesystem reads). Iterative
  development against `vmaf-tune corpus` becomes interactive.
- **Positive**: the row schema does not change. `SCHEMA_VERSION`
  stays at 1 and Phase B/C consumers see no delta.
- **Positive**: cache is opt-out, not opt-in. The user does not
  have to know the cache exists to benefit from it.
- **Negative**: disk pressure under heavy sweeps. Mitigated by the
  10 GiB default cap and `--cache-size-gb`. A sweep that produces
  more than 10 GiB of unique encodes will see its oldest entries
  evicted; this is correct LRU behaviour but worth flagging to
  users with very large grids.
- **Negative**: a corrupt cache directory (truncated blob, malformed
  meta JSON) currently surfaces as a miss-by-omission rather than a
  loud error. The miss is correct (we'll re-encode), but the
  underlying corruption is not surfaced. Acceptable trade-off for
  Phase A; revisit if it bites.
- **Neutral / follow-ups**:
  - A `vmaf-tune cache prune` subcommand would let users explicitly
    reclaim disk without waiting for the next `put`. Not shipped
    here; tracked as a low-priority follow-up.
  - The `adapter_version` string is currently a manual field on
    each adapter. A future change to the adapter contract could
    fold the argv shape into a hash automatically — out of scope
    for this PR.

## References

- ADR-0237 — vmaf-tune umbrella (parent).
- ADR-0028 — ADR-maintenance rule.
- ADR-0100 — project-wide doc-substance rule (the docs/usage update
  in this PR satisfies the per-surface bar).
- ADR-0108 — six deep-dive deliverables (all six accompany this PR).
- Source: user request 2026-05-03 — *"add a content-addressed cache
  to vmaf-tune so that re-running a tune with overlapping (src,
  encoder, preset, CRF) tuples doesn't re-encode and re-score"* with
  hard rules: do not bake cache content into JSONL rows, do not
  cache outside the requested cache-dir, do not skip the version
  components in the key (paraphrased per global user-quote rule).
