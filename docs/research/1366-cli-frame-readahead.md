<!-- markdownlint-disable MD013 MD060 -->
# Research-1366: Frame read-ahead in the `vmaf` CLI

- **Status**: Active
- **Workstream**: [ADR-1366](../adr/1366-cli-frame-readahead.md) (RC3 performance)
- **Last updated**: 2026-09-29

## Question

At 3840x2160 every backend showed a per-frame floor of about 7 ms that did not
depend on the feature: on an i9-12900K with an Arc B580, `psnr`, `float_psnr`,
`motion`, `adm` and `vif` on the B580 all measured 6.5 to 8.5 ms per frame, and
the CPU twins on 16 threads sat on the same floor whenever their extraction was
cheaper. Where does the floor come from, can it overlap scoring, and what must
stay unchanged when it does?

## Sources

- `core/tools/vmaf.cpp` on master `2d9d5b069`: `run_frame_loop()`,
  `fetch_picture()`, `copy_picture_data()`, `classify_frame_fetch()`,
  `release_unpaired_pictures()`, `skip_initial_frames()`,
  `preallocate_cli_pictures()`, `open_cli_inputs()`.
- `core/tools/vidinput.c`, `y4m_input.c`, `yuv_input.c`: the per-stream reader
  state (`video_input` owns its `FILE *` and its context; no shared statics).
- `core/src/picture_pool.cpp`: `vmaf_picture_pool_fetch()` blocks on a
  condition variable until a picture is released, and
  `pooled_picture_release()` signals it; `vmaf_picture_pool_close()` waits for
  every picture.
- `core/src/libvmaf.c`: `vmaf_read_pictures()`, `threaded_read_pictures_batch()`
  (a worker job holds the reference, the distorted picture and a counted
  snapshot of the previous reference), `read_pictures_update_prev_ref()` (the
  context keeps one previous reference between calls),
  `dispatch_gpu_double_buffer()`.
- [ADR-0104](../adr/0104-picture-pool-always-on.md),
  [ADR-1262](../adr/1262-cli-input-read-error-exit-code.md),
  [ADR-0520](../adr/0520-cli-no-reference-wiring.md).

## Findings

**The floor is the read, on the scoring thread, one stream after the other.**
`run_frame_loop()` called `fetch_picture()` for the reference, then for the
distorted stream, then `vmaf_read_pictures()`. Each fetch takes a pool picture,
`fread`s the frame into the reader's buffer and copies it plane by plane into
the picture: 12.4 MB twice per 4K 8-bit 4:2:0 frame. With `--feature psnr`
on 16 threads master spends about 7 ms per frame, which is the cost of those
two reads: the psnr work itself runs on the workers.

**The two streams can be read concurrently.** Each `video_input` owns its
`FILE *` and parse state; the Y4M and raw readers keep no mutable statics. The
picture pool is thread-safe. Only `vmaf_read_pictures()` has an ordering
contract (pairs in stream order, consecutive indices, one caller), so the
reads can move to other threads as long as that call stays where it is.

**Pool sizing decides whether a reader can deadlock.** Once its workers are
idle, libvmaf holds only the previous reference (plus, for a GPU extractor
with `submit`/`collect`, the reference it keeps for its pending frame). The
master pool is `2 * (threads + 1) + 1`. A reader that reserves a ring slot
before it takes a picture holds at most `kReadaheadDepth` pictures, so adding
`2 * kReadaheadDepth` to the pool keeps every picture libvmaf could hold before
and leaves at least one picture for a reader whenever the scorer waits on it.
A reader blocked on the pool while the other reader's ring is full is woken by
the scorer consuming that ring, or, at shutdown, by `request_stop()` draining
it; joining one reader before draining the other could wait forever.

**Two handles on one object must not be read in parallel.** Separately opened
regular files have separate offsets, but one pipe opened twice (for example
`/dev/stdin` on both sides) hands each read whichever bytes come next. The
serial loop gave the reference the even frames and the distorted stream the odd
ones; two threads would make that assignment depend on timing. `fstat()` device
and inode identify this on POSIX. `--no-reference` opens the distorted input
twice (ADR-0520), which the same test sends to the inline path. `_fstat64` on
Windows reports no inode, so there only two regular files are read in
parallel.

**What the readers change on stderr.** Scoring, `classify_frame_fetch()` and
the progress line stay on the main thread, so their output is unchanged. The
input readers print their own diagnostics (`Error reading YUV frame data.`),
and a reader running ahead can hit a damaged frame past the point where the
other stream ended; the message then appears on a run that exits 0. On a 4K
`--threads 16` default-model run the `est_params: covariance matrix was
singular on N of M solves` line differs between runs of master itself (12 or
16), because it sums per-thread SpEED contexts; it is not caused by read-ahead.

### Bit-exactness

JSON reports compared after removing the wall-clock `fps` field, master
`2d9d5b069` against the branch, same host and binaries' libvmaf.

| Case (CPU build) | Frames | Exit | JSON |
|---|---|---|---|
| Netflix 576x324 default model | 48 | 0 | identical |
| … default model + `--feature psnr`, serial and `--threads 4` | 48 | 0 | identical |
| … default model `--threads 16` | 48 | 0 | identical |
| … `--feature psnr --no_prediction` | 48 | 0 | identical |
| … `--frame_cnt 10`, serial and `--threads 4`; `--frame_cnt 1` | 10, 10, 1 | 0 | identical |
| … `--frame_skip_ref 3 --frame_skip_dist 1` | 45 | 0 | identical |
| … `--subsample 3` | 16 | 0 | identical |
| Netflix 10-bit 4:2:0 `--feature psnr` | 3 | 0 | identical |
| Y4M (Netflix pair wrapped) `--feature psnr`; default model `--threads 8` | 48 | 0 | identical |
| Y4M reference truncated mid-frame 12 | — | 102 | no report, both |
| Raw distorted truncated mid-frame 30 (rejected by the size check) | — | 2 | no report, both |
| Distorted ends after 20 frames; reference ends after 20 frames | 20 | 0 | identical |
| Same file on both sides (inline path) | 48 | 0 | identical |
| BBB 3840x2160, 50 frames, default model + psnr `--threads 16` | 50 | 0 | identical |
| BBB 3840x2160, 50 frames, `float_psnr` serial | 50 | 0 | identical |

| Case (SYCL build, Arc B580, `level_zero:0`) | Frames | JSON |
|---|---|---|
| Netflix 576x324 `--backend sycl` `psnr`, `vif`, default model | 48 | identical |
| BBB 3840x2160 `--backend sycl` `psnr`, `vif`, default model | 50 | identical |

Stderr was byte-identical in every CPU case except the SpEED line above.

### Timing

Host: i9-12900K, Arc B580, Docker Desktop WSL2 VM with 22 logical CPUs, shared
with other agents' builds (load average 6 to 14 during the runs). Input: BBB
3840x2160 8-bit 4:2:0, page-cached. Master and branch ran back to back in each
repetition; the tables give the median and the min..max spread in ms per
frame.

**In-loop, 61 frames, 5 repetitions, two sessions.** The CLI runs under a
pseudo-terminal and the last FPS on its progress line is read: that value is
frames divided by the wall time since the frame loop started, so device init,
model load and pool allocation are excluded. GPU runs take `flock
/f/gpu.lock` for the whole run. Session A ran at load average 6 to 14. Session
B ran the CPU rows at load 3 to 9 and the GPU rows after a Docker Desktop
restart had stopped the other containers (load 0.5 rising to 7.6).

| Backend, `--threads` | Feature | A: master | A: branch | B: master | B: branch | B speedup |
|---|---|---|---|---|---|---|
| CPU, 16 | `psnr` | 9.23 [8.22..11.35] | 4.05 [3.63..5.08] | 6.90 [6.70..7.38] | 3.41 [3.23..3.75] | 2.02x |
| CPU, 16 | `float_psnr` | 10.16 [8.11..12.72] | 6.83 [5.85..8.09] | 7.44 [6.67..7.64] | 5.11 [4.68..6.19] | 1.46x |
| CPU, 16 | default model | 20.21 [19.93..24.99] | 20.32 [18.72..21.32] | 21.45 [19.00..26.94] | 20.51 [17.30..23.33] | 1.05x |
| CPU, 0 | `psnr` | 8.20 [7.70..12.83] | 4.22 [3.54..5.85] | 8.19 [7.65..8.88] | 3.97 [3.64..4.25] | 2.06x |
| CPU, 0 | `float_psnr` | 19.29 [18.52..21.15] | 13.54 [12.85..14.35] | 17.59 [17.32..17.99] | 12.01 [11.07..12.26] | 1.46x |
| SYCL B580, 0 | `psnr` | 10.19 [9.76..11.19] | 5.63 [5.24..6.10] | 7.81 [7.44..8.10] | 4.13 [3.99..4.22] | 1.89x |
| SYCL B580, 0 | `float_psnr` | 10.99 [9.62..16.37] | 5.56 [5.49..5.91] | 7.71 [7.15..9.45] | 4.45 [4.37..5.55] | 1.73x |
| SYCL B580, 0 | `motion` | 9.12 [8.71..11.22] | 5.11 [4.46..5.95] | 7.42 [7.10..8.04] | 3.79 [3.29..4.14] | 1.96x |
| SYCL B580, 0 | `vif` | 9.96 [9.40..11.44] | 7.55 [7.27..7.87] | 8.49 [7.92..9.03] | 6.79 [6.64..7.05] | 1.25x |
| SYCL B580, 0 | `adm` | 9.42 [8.74..10.60] | 5.00 [4.29..6.05] | 8.59 [7.50..9.59] | 4.60 [4.29..5.27] | 1.87x |
| SYCL B580, 0 | default model | 54.59 [51.07..73.37] | 51.36 [49.83..62.11] | 63.73 [52.60..74.52] | 59.88 [50.38..61.46] | 1.06x |

In session B the branch was faster than master in every repetition of every
row except one repetition of the SYCL default model. The `--feature` names on
`--backend sycl` run the SYCL twins (ADR-1359). With the default model the JSON
`feature_backends` lists `cambi_sycl`, `speed_chroma_sycl` and `motion_sycl` on
the device and `adm` on the CPU (the model's ADM options keep the CPU
extractor), so that row is limited by serial CPU ADM, not by reading.

Session C (load average 0.8 to 1.6), same method; the UHD 770 is the
i9-12900K's integrated GPU (`level_zero:1`):

| Backend, `--threads` | Feature | master | branch | speedup |
|---|---|---|---|---|
| CPU, 16 | `motion` | 4.72 [4.55..5.40] | 2.84 [2.72..3.15] | 1.66x |
| SYCL UHD 770, 0 | `psnr` | 24.84 [24.06..26.25] | 20.63 [20.40..20.88] | 1.20x |
| SYCL UHD 770, 0 | `motion` | 13.07 [12.56..14.17] | 9.65 [9.57..10.52] | 1.35x |
| SYCL UHD 770, 0 | default model | 70.67 [69.54..72.31] | 67.29 [65.32..68.07] | 1.05x |

The integrated GPU shares memory bandwidth with the CPU and computes more
slowly, so the read cost is a smaller share of each frame; the branch still
saves 3.4 to 4.2 ms per frame there, faster in every repetition.

**Startup-difference method, 3 repetitions**: wall time of `--frame_cnt 22`
minus `--frame_cnt 2`, divided by 20. It agrees on the CPU; on the GPU the
device start-up varies by hundreds of milliseconds between runs, which puts
several ms of noise on each estimate (some repetitions came out negative), so
the in-loop numbers above are the GPU result.

| Backend, `--threads` | Feature | master | branch |
|---|---|---|---|
| CPU, 16 | `psnr` | 6.99 [6.91..7.17] | 3.31 [2.61..3.37] |
| CPU, 16 | `float_psnr` | 8.13 [7.70..9.03] | 6.01 [5.53..6.17] |
| CPU, 16 | default model | 22.16 [21.89..23.05] | 24.22 [22.18..25.67] |
| CPU, 0 | `psnr` | 7.27 [6.97..7.45] | 3.55 [3.36..3.69] |
| CPU, 0 | `float_psnr` | 16.04 [15.87..16.54] | 11.62 [11.60..11.81] |

The CPU default model on 16 threads is limited by extraction, not by reading.
Its 22-frame estimate is dominated by start-up variance: five more repetitions
gave master 24.85 [-13.96..27.86] and branch 27.95 [23.11..32.56], and a
100-frame window (`--frame_cnt 102` minus 2) gave master 22.41
[14.30..24.35] and branch 21.28 [20.29..21.94]. With the in-loop method the two
are equal within the spread.

With read-ahead the single-feature GPU runs settle at 3.8 to 4.6 ms per frame
in session B (`vif` at 6.8). The two readers are no longer the limit there
(each reads one 12.4 MB frame per step in parallel); what remains is on the
scoring thread: the host-to-device upload of the pair inside
`vmaf_read_pictures()` and the wait for the device result. This split was not
profiled.

### Queue depth

The same branch built with `kReadaheadDepth` 1 and 3, each interleaved with
depth 2 (in-loop method, 61 frames, 5 repetitions, load average below 2.5):

| Backend, `--threads` | Feature | depth 2 | depth 1 | depth 3 |
|---|---|---|---|---|
| CPU, 0 | `psnr` | 3.10 [2.88..3.35] / 3.01 [2.93..3.20] | 3.40 [2.96..3.83] | 3.26 [2.96..3.40] |
| CPU, 16 | `float_psnr` | 4.29 [4.23..4.98] / 4.53 [4.26..4.84] | 4.25 [4.21..4.35] | 4.47 [4.13..4.76] |

(Depth 2 was measured once against each variant, hence two entries.) Depth 1
was slower than depth 2 in four of five serial `psnr` repetitions; depth 3
gained nothing and costs two more pool pictures. Depth 2 it is.

## Alternatives explored

- **One producer thread reading the reference and then the distorted frame.**
  It would keep the serial byte order even for a shared pipe, but the two reads
  stay serial. Master already spends about 7 ms per 4K frame on them with
  `--feature psnr --threads 16`, where the extraction is on the workers, so a
  single producer could not go below that; two readers reach 3.5 to 4 ms.
- **`USE_DIRECT_READ`** (`video_input_fetch_into_vmaf_picture()`): removes the
  copy into the picture but keeps the read on the scoring thread, and the Y4M
  path rejects the formats that need conversion. It is independent of this
  change and could be combined with it.
- **Detaching the readers on an error** instead of joining them: a detached
  reader would still use the picture pool and its `video_input` after
  `vmaf_close()`. Joining costs at most one frame read.
- **An opt-out flag**: outputs do not depend on the path, so it would add a CLI
  surface without a determinism reason.

## Open questions

- The upload inside `vmaf_read_pictures()` is now the GPU floor. Overlapping it
  with the previous frame's kernels needs double-buffered device pictures in
  the backend, not a CLI change.
- `USE_DIRECT_READ` on top of read-ahead would remove one copy per frame on the
  reader threads; not measured.

## Related

- [ADR-1366](../adr/1366-cli-frame-readahead.md), this change.
- [ADR-0104](../adr/0104-picture-pool-always-on.md), picture pool.
- [ADR-1262](../adr/1262-cli-input-read-error-exit-code.md), exit codes the
  readers must preserve.
- `core/tools/test/test_vmaf_frame_readahead.sh`.
