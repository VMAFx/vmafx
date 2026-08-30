# ADR-0147: Thread-pool job-object recycling + inline data buffer

- **Status**: Accepted
- **Date**: 2026-04-24
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: performance, threading, upstream-port

## Context

The fork's `core/src/thread_pool.c` allocates and frees a
`VmafThreadPoolJob` *and* a separate payload buffer on every
`vmaf_thread_pool_enqueue` call. Callers submit tiny payloads
(`struct { VmafPicture ref, dist; ... }` in the main extractor path,
~48 bytes in the MCP frame-event path). For a 120-frame 1080p run
with multi-feature extraction, this comes out to thousands of
paired `malloc` / `free` calls per wall-second inside the hot path
`vmaf_read_pictures → threaded_read_pictures → vmaf_thread_pool_enqueue`
chain.

Netflix upstream PR [#1464](https://github.com/Netflix/vmaf/pull/1464)
(Tolga Kilicli, Feb 2026 — closed without merge) bundled twelve
unrelated optimizations. Two of them apply cleanly to the fork's
thread-pool:

1. **Job-object free list** — recycle `VmafThreadPoolJob` slots via a
   pool-local linked list rather than `malloc`/`free` on every job.
2. **Inline data buffer** — payloads ≤ 64 bytes are copied into a
   fixed-size `char inline_data[64]` array at the tail of the job
   struct, eliminating the second allocation.

The rest of Netflix #1464 (PSNR AVX2, ADM micro-opts, VIF epsilon
removal, predict.c refactor, feature-collector capacity bump,
convolution stride hoisting, comprehensive test suite) either
overlaps with fork-local work that has already landed (T3-4 motion,
T7-5 predict refactor, ADR-0142 VIF, 81fcd42e PSNR SIMD) or directly
conflicts with ADR-0138/0139 bit-exactness invariants. A narrow port
is the right shape.

The fork's thread-pool carries two extensions relative to upstream
that must survive the port:

- `VmafThreadPoolWorker` struct with per-worker `void *data` plus a
  `thread_data_free` destructor callback. Callers submit work via a
  `func(void *data, void **thread_data)` signature, not upstream's
  `func(void *data)`.
- `pthread_cond_signal` (not `broadcast`) on empty → already
  incorporated at `thread_pool.c:175` as of master. Upstream PR #1464
  claimed a "thundering-herd fix" ported over the same `broadcast →
  signal` change; the fork has it already.

## Decision

Port the job-object free list + inline data buffer from
Netflix #1464, adapted to the fork's `void **thread_data` signature:

1. Add `#define JOB_INLINE_DATA_SIZE 64` and embed
   `char inline_data[JOB_INLINE_DATA_SIZE]` in `VmafThreadPoolJob`.
2. Add a `VmafThreadPoolJob *free_jobs` list to `VmafThreadPool`,
   protected by the existing `queue.lock`.
3. Split the cleanup path into
   `vmaf_thread_pool_job_clear_data` (drops heap-allocated payload if
   and only if it isn't pointing at the inline buffer) +
   `vmaf_thread_pool_job_destroy` (legacy free-the-whole-thing path,
   used only on pool teardown for leaked slots) +
   `vmaf_thread_pool_job_recycle` (new — pushes a finished slot onto
   the free list).
4. `vmaf_thread_pool_runner` now calls `_recycle` instead of
   `_destroy` after running a job.
5. `vmaf_thread_pool_enqueue` now takes the queue lock *before*
   allocating the slot; it pops from `free_jobs` if non-empty,
   otherwise `malloc`s a new slot. Payloads ≤ 64 bytes are copied
   into `job->inline_data` and `job->data` is pointed at that
   buffer; larger payloads keep the legacy `malloc` path.
6. `vmaf_thread_pool_destroy` walks the `free_jobs` list after
   `vmaf_thread_pool_wait` returns (all workers have exited, so
   no locking needed) and frees every recycled slot.

Bit-exactness and throughput are both load-bearing:

- Bit-exactness: scalar vs SIMD (VMAF_CPU_MASK=0 vs =255) remains
  byte-identical under `--threads 4` on the Netflix golden pair
  (`diff` exit 0).
- Throughput: a 500 000-job micro-benchmark (4 worker threads,
  `int v = 1` payload per job) shows ~1.8–2.6× speedup:

  | Config | Median throughput |
  | --- | --- |
  | Before (master) | ~1.20 M jobs/sec |
  | After (this PR) | ~2.20 M jobs/sec |

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Full Netflix #1464 port** | One-shot sweep of all twelve upstream optimizations | Direct conflict with ADR-0138 / 0139 (VIF epsilon removal, ADM bit-shift aggression), with T7-5 predict.c refactor, with the fork's feature-collector extensions, and with the fork's existing 81fcd42e PSNR SIMD (already more complete than upstream's AVX2-only path) | Rejected — narrow port avoids re-litigating already-decided invariants; the omitted pieces are either done, unsafe, or out of scope |
| **Job-pool only, no inline buffer** | Smaller patch, removes only one `malloc`/`free` pair per job | Callers with small payloads (the common case) still do a second malloc for every enqueue | Rejected — the inline buffer is where the bulk of the throughput win comes from; job recycling alone is about 1.3× |
| **Heap-backed object pool with a max-size cap** | Could bound memory growth under adversarial enqueue bursts | Current queue already provides natural backpressure via `n_working` + `queue` → no unbounded growth in practice; a cap adds mutex-held branch-heavy fallback code with no measurable win | Rejected — match upstream's simpler unbounded-growth-then-free-on-destroy strategy |
| **Thread-local slab allocator** | Zero lock contention on the pool lock for allocations | Current pool lock is already held while touching the queue; the allocation-under-lock pattern does not serialize any real work that wasn't already serialized | Rejected — premature optimization. Revisit if profile-hotpath shows lock contention later |

## Consequences

- **Positive**:
  - ~2× enqueue throughput in the micro-benchmark. On a
    thread-pool-heavy run (multi-feature, 1080p, `--threads 4`) this
    drops the allocator's share of the per-job cost from "visible in
    flamegraph" to "disappears into the noise floor".
  - Payload copies now go through a predictable inline buffer for
    the common case — one less cache-miss class to worry about in
    subsequent profiling.
  - `vmaf_thread_pool_destroy` remains leak-free under all exit paths
    (normal shutdown, early stop) — every recycled slot is walked
    and freed after the workers stop.
- **Negative**:
  - `sizeof(VmafThreadPoolJob)` grows from 24 bytes to 88 bytes
    (`func 8 + data 8 + inline_data 64 + next 8`). Each long-lived
    pool pays `64 * peak_jobs_in_flight` bytes more. A 4-thread
    pool in practice carries a handful of slots alive at any time;
    the trade is trivial relative to even one 1080p frame.
  - Enqueue now acquires `queue.lock` before allocation (previously
    it allocated first, then locked). Allocations under the lock
    can theoretically stretch the critical section on a cold cache
    or heavy fragmentation; in the benchmarked workload throughput
    strictly improved, so the concern is academic for now.
- **Neutral / follow-ups**:
  - If profile shows the 64-byte buffer is undersized for a future
    consumer, bump `JOB_INLINE_DATA_SIZE` and recompile — no API
    change.
  - Netflix #1464 is still open upstream. If it ever lands, keep
    the fork's version on conflict (fork's `void **thread_data`
    signature is required for the fork's per-worker data path —
    see [`core/src/AGENTS.md`](../../core/src/AGENTS.md)).

## References

- Upstream source:
  [Netflix/vmaf#1464](https://github.com/Netflix/vmaf/pull/1464),
  specifically the thread-pool portion of commit `ad2b90c3`
  ("Add AVX2 PSNR, thread pool job pool, and comprehensive tests").
- Backlog item: `.workingdir2/BACKLOG.md` T3-6 (the AVX2-PSNR half
  was already covered by fork commit `81fcd42e`; this PR closes the
  thread-pool half).
- [ADR-0141](0141-touched-file-cleanup-rule.md) — applied to
  `thread_pool.c`: zero clang-tidy warnings on the touched file, no
  NOLINT.
- User direction 2026-04-24 popup: "Port thread-pool job-pool
  (Recommended)" after T7-5 merged.
