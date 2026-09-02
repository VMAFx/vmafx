# ADR-0862: K150K extractor — .done vs parquet consistency check on restart

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `ai`, `pipeline`, `durability`, `k150k`

## Context

`ai/scripts/extract_k150k_features.py` extracts VMAF-CPU features for the
152 265-clip K150K corpus and persists them as a single parquet file. Three
durability mechanisms have grown up around it:

1. A `<out>.done` checkpoint — JSONL of completed clip names, appended one
   line at a time, fsync-durable per `_append_done`.
2. A `<out>.rows.jsonl` staging file — append-only buffer of completed
   rows, written one line at a time so a crash mid-run does not lose
   already-extracted features.
3. The final `<out>.parquet` — written exactly once at the end of `main()`
   via `_write_parquet_from_rows` (rename(2)-then-unlink), then the
   staging file is unlinked.

The 2026-05-30 RCA for Bug-3 found that the `.done` checkpoint had grown
to 152 265 entries while the on-disk parquet held only 59 812 rows — a
silent loss of ~92 K rows. The mechanism:

- A prior K150K run was killed (OOM / operator interrupt) **after** the
  parquet write at L1406 completed but **before** the staging-file
  `unlink(missing_ok=True)` at L1408 — or, more likely, after a partial
  parquet rename(2) into place but before fsync, on a filesystem that
  reordered the rename ahead of the data writes.
- The next invocation observed `pending == []` (because `.done` already
  covered every clip in `--clips-dir`) and fell into the no-op early-exit
  branch at L1310–L1329.
- That branch unconditionally wrote a `status=complete-noop` manifest and
  returned 0. It never compared `.done` count to parquet row count.
- The operator saw "nothing to do, complete" and trusted the (truncated)
  parquet downstream.

The same branch will be entered every time the script is re-run, because
the gap between `.done` and the parquet is permanent unless somebody
truncates `.done` or re-extracts. The silent-confirmation property meant
the failure was discoverable only by counting parquet rows by hand.

## Decision

We will treat the `.done` checkpoint as the authoritative ledger of which
clips have been processed, and require the no-op early-exit branch to
fail closed (`RuntimeError`) when the parquet row count plus any
recovered staging rows is less than `len(done_set)`. The end-of-run
write path gets a matching row-count sanity check (recovered + ok must
equal the final row list length) and an explicit `fsync` of the parquet
file and its parent directory before the staging file is unlinked.

The four concrete changes shipped together:

1. **`_load_staging_rows` surfaces a malformed-line count** — replaces
   the silent `JSONDecodeError: continue` with a stderr WARNING reporting
   skip count and recovered count. A truncated staging tail is a leading
   indicator of the same crash class.
2. **No-op branch consistency check** — at L1310, after recovering any
   staging rows, compare `len(done_set)` against parquet row count
   (plus recovered if not already merged). On mismatch, raise
   `RuntimeError` naming the missing-clip count and the recovery hint
   ("remove the affected entries from `.done` or delete it to
   re-extract everything").
3. **End-of-run row accounting assert** — before writing the parquet at
   L1406, verify `len(rows) == len(recovered_rows) + ok`. On mismatch,
   raise `RuntimeError` and **preserve the staging file** for forensic
   recovery instead of letting the broken parquet land.
4. **fsync before unlink** — call `_fsync_path(args.out)` after the
   parquet rename and before the staging unlink, both in the no-op
   branch and in the end-of-run write path. The helper fsyncs the file
   then the parent directory so the rename(2) is durable before the
   companion unlink can race ahead of it on power loss.

Operator workflow on the existing 92 K-row deficit: delete (or trim) the
`.done` file's missing entries and re-run; the new no-op-branch guard
will refuse to silently confirm any future mismatch.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Status quo — no consistency check, rely on operator vigilance | Zero code | Already failed in production (~92 K rows lost without detection); operator vigilance is exactly what failed | Lossy-silent is unacceptable for a multi-week extraction job |
| Periodic parquet writes (every N clips) instead of write-once-at-end | Smaller blast radius per crash | Reverts Research-0135 Win 1 (pandas DataFrame allocation dominates wall-time when re-run repeatedly); doesn't fix the no-op branch trust gap | The staging-JSONL design is correct; the bug is the missing post-condition check |
| Replace JSONL staging with sqlite WAL | True transactional durability per row; row count is queryable in O(1) | New dependency in the AI script tree; sqlite locking interacts badly with the ProcessPoolExecutor row append pattern; large schema-migration cost | Costs are outsized for a one-shot per-corpus extraction; existing JSONL is fine *with* a consistency check |
| Auto-truncate `.done` to match parquet on mismatch | Re-runnable without operator intervention | Hides the loss; rewards the same antipattern that caused the bug | Loud-fail is the explicit goal — the operator must know rows were lost |
| Add fsync only (skip consistency check) | One-liner; covers future crashes | Doesn't detect or recover from the *existing* 92 K-row deficit; future operator silence still possible if fsync isn't reached | Both are needed: fsync prevents the next loss, consistency check catches the loss that already happened |

## Consequences

- **Positive**:
  - Silent row loss is now structurally impossible on restart — the no-op
    branch refuses to confirm a mismatched corpus.
  - End-of-run accounting catches in-process row-bookkeeping bugs (the
    `recovered_rows` merge logic is non-trivial; the assert pins its
    invariant).
  - fsync before unlink prevents the next instance of the same crash
    class, even on filesystems that reorder rename ahead of data writes
    (ext4 with `data=writeback`, NFS without `sync` mount option).
  - Malformed-staging WARNING surfaces leading indicators an operator
    can act on (e.g. an in-progress run was killed during the final
    JSONL line write).

- **Negative**:
  - The no-op branch is now slower by one `pd.read_parquet(columns=["clip_name"])`
    pass on every restart. For 152 K rows the cost is ~1–2 s on local
    NVMe — negligible against a multi-week extraction wall-time.
  - The existing 92 K-row deficit needs operator action (truncate `.done`
    and re-extract) before the script is runnable on the current corpus;
    the script will refuse to silently no-op until that's done. This
    is intentional — silent no-op is the antipattern.
  - `_fsync_path` is best-effort: it catches `OSError` and warns, so
    on tmpfs / FUSE / overlayfs it degrades gracefully but the
    durability guarantee weakens. Production K150K runs on local ext4
    where fsync is real.

- **Neutral / follow-ups**:
  - Unit test added: `ai/tests/test_extract_k150k_consistency.py` —
    four cases (consistency-error raise, negative control,
    malformed-line WARNING, fsync no-raise on missing).
  - Operator runbook update: when restarting after a crash on the
    current K150K corpus, expect the `RuntimeError` and follow the
    recovery hint in the error message.
  - The K150K extraction currently in flight (PID 307050, 98.3 %
    complete as of 2026-05-30) must complete before this fix lands.
    Once landed, the next operator-initiated restart will surface
    the historical deficit.

## References

- Bug-3 RCA: 2026-05-30 investigation by user; `.done` count 152 265
  vs parquet 59 812 rows on shared K150K parquet output.
- Source: `req` (operator-provided RCA sketch and fix plan,
  2026-05-30 13:54 UTC) — paraphrased: "surface JSONDecodeError counts,
  add a `.done`-vs-parquet consistency check on restart, sanity-check
  at end-of-run, and don't unlink staging before parquet is fsync'd."
- Related: [Research-0135](../research/) (parquet write-once-at-end
  optimisation — preserved; this ADR adds a durability gate on top of
  it, not in place of it).
- Related ADRs: ADR-0042 (tiny-AI docs bar; the operator-facing
  failure mode is documented in `docs/ai/k150k-extraction.md` as
  part of the same PR).
