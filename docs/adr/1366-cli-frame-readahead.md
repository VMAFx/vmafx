<!-- markdownlint-disable MD013 MD060 -->

# ADR-1366: The `vmaf` CLI reads each input on its own thread, a bounded number of frames ahead

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: Lusoris
- **Tags**: cli, performance, threading, fork-local

## Context

`run_frame_loop()` in `core/tools/vmaf.cpp` fetched the reference frame, then
the distorted frame, then handed the pair to `vmaf_read_pictures()`, all on the
main thread. Each `fetch_picture()` takes a picture from libvmaf's picture pool,
reads the frame from the file and copies it into the picture. At 3840x2160
8-bit 4:2:0 that is 12.4 MB per frame and 24.9 MB per pair, and the fetch was a
fixed cost of about 7 ms per frame whatever the backend did with the pair. On
an i9-12900K with an Arc B580, startup-free per-frame costs were 9.15 ms for
`psnr` on 16 CPU threads and 8.5 ms on the B580, 6.95 and 6.75 for `float_psnr`,
7.0 for `motion` and `vif` on the B580: the GPU twins sat on the read floor,
and so did every CPU run whose extraction was cheaper than the read. While the
main thread read, nothing was scored in the serial case, and in every case the
reference and the distorted stream were read one after the other.

The two streams are independent: each `video_input` owns its `FILE *` and its
parse state (`y4m_input.c`, `yuv_input.c`), and the picture pool is already
thread-safe (`picture_pool.cpp`, mutex plus condition variable). Only the
scoring side has an ordering contract: `vmaf_read_pictures()` must receive the
pairs in stream order with consecutive indices, from one thread.

## Decision

The CLI gives each input stream a `FrameReader`. When the two inputs can be
read independently, the reader runs `fetch_picture()` on its own `std::thread`
and queues each result in a fixed ring of `kReadaheadDepth = 2` slots; the
scoring loop pops one frame from each reader per step and otherwise runs as
before on the main thread. Specifically:

- **Order and scores.** The scoring loop still makes every
  `vmaf_read_pictures()` call, in the same order, with the same index, from the
  same thread. Each reader delivers its stream's `fetch_picture()` results in
  stream order, and `classify_frame_fetch()` still judges each pair, so the
  frames scored, their pairing, the progress line, the `ended before` warning
  and the ADR-1262 exit codes are unchanged. JSON output is identical at
  `--precision max`.
- **Bounds.** A reader reserves a ring slot before it takes a pool picture, so
  it never holds more than `kReadaheadDepth` pictures, and it reads at most
  `--frame_cnt` frames (`UINT_MAX` when unset), stopping after the first frame
  that ends or fails its stream. The frame loop is bounded by the same limit.
- **Picture pool.** `preallocate_cli_pictures()` adds `2 * kReadaheadDepth`
  pictures to the pool when read-ahead is on, so the readers never draw on the
  pictures the pool was sized for (`2 * (threads + 1) + 1`). A reader blocked on
  an empty pool is always woken: libvmaf holds at most the previous reference
  once its workers are idle, and the pool always has room for one picture per
  reader beyond that.
- **Shutdown.** After the loop, `request_stop()` is called on both readers
  before either is joined. It sets the stop flag, drains the ring and returns
  each queued picture to the pool, which is what wakes a reader blocked in
  `vmaf_fetch_preallocated_picture()`. A reader that finishes a frame after the
  stop returns that picture itself. Every picture a reader took is back in the
  pool before `vmaf_read_pictures(NULL, NULL)` flushes the context.
- **Inline fallback.** A reader without a thread calls `fetch_picture()` from
  `next()`, which is the pre-ADR-1366 loop exactly. It is used when the inputs
  might share a read position: on POSIX when both `FILE *` name the same
  device and inode (one pipe opened twice, `-r x -d x`, and `--no-reference`,
  which opens the distorted input twice); on Windows unless both are regular
  files, since `_fstat64` reports no inode there. It is also used when the
  thread cannot be created.
- **No flag.** Scores do not depend on which path runs, so there is no opt-out.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One reader thread per stream, depth 2 (chosen) | Reads the two streams in parallel and overlaps them with scoring; one implementation used twice; no library change | Two threads; four more pool pictures | — |
| One producer thread fetching ref then dist | Keeps today's byte order of reads even on a shared pipe | The two reads stay serial: master already spends about 7 ms per 4K frame on them with `--feature psnr --threads 16`, so a single producer cannot go below that, where two readers reach 3.8 ms | Leaves the read floor in place |
| Read directly into the picture (`USE_DIRECT_READ`) | Removes one 12.4 MB copy per frame | Still on the scoring thread and still serial; the Y4M conversion formats are not supported by that path | Orthogonal; can be combined later |
| Depth 1 | Two extra pictures instead of four | A reader cannot start the next frame until the scorer has taken the previous one | Slower than depth 2 in four of five serial `psnr` repetitions (3.40 against 3.10 ms per 4K frame, Research-1366) |
| Depth 3 | Absorbs longer scoring stalls | Six extra pictures (about 75 MB at 4K 8-bit) | No gain over depth 2 on serial `psnr` or on `float_psnr --threads 16` (Research-1366) |
| `--readahead N` / opt-out flag | Lets a user trade memory for speed | A new CLI surface with docs, parser, fuzz corpus and MCP mirrors, for a setting whose outputs are identical | Not needed for determinism; the inline path already covers shared inputs |
| Detach reader threads on error instead of joining | Never waits for a slow pipe | A detached thread would still touch the pool and the `video_input` after `vmaf_close()` | Unsafe; join is bounded by one frame read |

## Consequences

- **Positive**: at 4K the per-frame read cost overlaps scoring and is split
  across two threads. `--feature psnr` falls from about 7 ms to about 3.5 ms per
  frame on the CPU, serial or with 16 threads; runs limited by extraction keep
  their speed. Measurements, method and spreads are in
  [Research-1366](../research/1366-cli-frame-readahead.md).
- **Negative**: the picture pool grows by four pictures when read-ahead is on
  (about 50 MB at 3840x2160 8-bit 4:2:0, about 100 MB at 10-bit), and the
  `picture pool: N pictures pre-allocated` line on a terminal shows the larger
  count. A reader can reach a damaged frame before the scoring loop stops at
  the end of the other stream; the input reader's own message (for example
  `Error reading YUV frame data.`) is then printed although the run exits 0 as
  before. After a `vmaf_read_pictures()` failure a reader that is inside a
  blocking read of a pipe finishes that one read before the process exits.
- **Neutral / follow-ups**: `core/tools/test/test_vmaf_frame_readahead.sh`
  pins frame order, pairing, `--frame_cnt` (including that no reader reads
  past it), `--frame_skip_dist`, the inline path, an early end and a failed
  read. ThreadSanitizer runs of the CLI are recorded in Research-1366.

## References

- Maintainer brief, 2026-09-29: "Overlap frame fetching with scoring: a bounded
  read-ahead [...] with a small fixed queue depth (2-3), clean shutdown on
  EOF/error/`--frame_cnt`, identical frame order and identical outputs".
- [Research-1366](../research/1366-cli-frame-readahead.md).
- [ADR-0104](0104-picture-pool-always-on.md) (picture pool sizing),
  [ADR-1262](1262-cli-input-read-error-exit-code.md) (read-error exit codes),
  [ADR-0520](0520-cli-no-reference-wiring.md) (`--no-reference` opens the
  distorted input twice), [ADR-0809](0809-cli-cpp23-conversion.md) (C++23 CLI).
