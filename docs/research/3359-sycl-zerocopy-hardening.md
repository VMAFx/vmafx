<!-- markdownlint-disable MD013 MD041 MD060 -->
# Research-3359: SYCL zero-copy hardening on the luma-only path

Companion digest for [ADR-3359](../adr/3359-sycl-primary-queue-immediate-cmdlist.md)
and [ADR-3360](../adr/3360-sycl-filter-twin-routing.md). It records which parts
of the Tualua fork's zero-copy work (its ADR-1595 to ADR-1599) were ported to
`VMAFx/vmafx` master after
[ADR-1688](../adr/1688-sycl-zero-copy-luma-only-admission.md), which parts were
left out, and the FFmpeg frame-count finding.

## What the fork found

On an Intel Arc A380, FFmpeg's `libvmaf_sycl` filter on QSV-decoded
(zero-copy) frames had five independent defects:

1. a CPU extractor named by `feature=` was skipped without an error;
2. SYCL twins that need host pictures dereferenced NULL or returned `-EINVAL`,
   and `motion_sycl` with `motion_add_uv` added chroma it never imported;
3. the filter never asked `vmaf_feature_backend_twin()` for a SYCL twin, so CPU
   names ran on the CPU (host upload) or failed (zero-copy);
4. under batched Level Zero command lists the driver silently dropped the
   per-frame VA import (`cambi` differed from host upload in 3 to 7 of 10 runs);
5. a failed VA import warned and skipped the frame.

Two test and API races came out of the investigation:
`test_sycl_ordered_sum_probe.cpp` freed the host sources of a deferred
`memcpy` before the wait, and `vmaf_sycl_upload_plane()` returned before its
copy had run while the D3D11 import released the source at once.

## Reconciliation with master

| Fork item | Master on `52e265fc0` | Ported? |
| --- | --- | --- |
| CPU extractor pre-pass guard (1) | ADR-1688 admission check refuses it by name before the frame is counted | No: covered |
| Per-extractor `-ENOTSUP` for missing host pictures (2) | ADR-1688 refuses every extractor whose `reads_shared_luma_only()` is absent or false, so no twin sees a NULL picture | No: covered (ADR-1688 rejected the per-twin form) |
| `feature=` twin routing in patch 0005 (3) | not present | Yes, ADR-3360 |
| NV12 / P010 guard on QSV input | not present | Yes, ADR-3360 |
| Immediate command lists on the primary queue (4) | not present | Yes, ADR-3359 |
| Fatal VA import (5) | open PR VMAFx/vmafx#2110 decides it differently (retry, then fail naming the frame; per-input VA display) | No: deferred to #2110 |
| Ordered-sum probe source lifetime | not present (`test_sycl_ordered_sum` fails under `MALLOC_PERTURB_` on master too) | Yes |
| `vmaf_sycl_upload_plane()` synchronous | fixed on master by `fix/sycl-upload-plane-fence` (row `T-SYCL-UPLOAD-PLANE-NO-COMPUTE-FENCE-2026-10-05`): orders the copy after the slot's readers and waits for it | No: covered |
| Chroma import, shared-plane twins, `float_motion` `motion_add_uv` (fork ADR-1597 to 1599) | deferred to post-1.0 by ADR-1688 / [ADR-1685](../adr/1685-post-1-0-embedding-zero-copy-milestone.md) | No: separate post-1.0 change |
| e2e harness and comparator | not present | Yes; stage 1 is ADR-1688's admitted set, stages 2 and 3 must be refused by name |

## Primary-queue command-list mode (ADR-3359)

Measured on the fork (Arc A380, i915, compute-runtime 26.35.39758.10, Level
Zero 1.34.0), debug session `sycl-zerocopy-cambi-nondeterminism`:

| Configuration | Runs with a frozen import |
| --- | --- |
| Batched command lists everywhere (master) | 3 to 7 of 10 (`cambi`, src01) |
| Wait on every queue before the free | 3 of 12 |
| Delayed free (fresh address per frame) | 0 of 12 |
| Per-surface import cache | 0 of 12 (+7 % ms per frame at 1080p) |
| Immediate command lists globally | 0 of 12 |
| Immediate primary queue only | 0 of 12 |
| Immediate import-only queue beside a batched primary queue | 8 of 12 |

1080p `vmaf_v0.6.1` zero-copy fps, 10 ABBA rounds: 80.92 to 82.64 (8 bit),
80.65 to 79.52 (10 bit), within noise.

## Frame count with `-frames:v` / `-t` (not a fork bug)

Reported as `libvmaf_sycl` (QSV decode, `hwdownload`, host upload) scoring
201 frames for `-frames:v 200` while CPU `libvmaf` scored 200. Repro on the
fork (`b1ffe0126`, Arc A380, FFmpeg n9.0.2 with the series), 12 runs per row,
software-decoded Netflix 576x324 pair (48 frames):

| Command | `libvmaf` frames scored | `libvmaf_sycl` frames scored |
| --- | --- | --- |
| `-frames:v 20` | 21 21 20 21 20 21 20 20 20 21 21 21 (7 of 12 score 21) | 21 20 20 20 20 21 21 20 21 20 20 20 (4 of 12) |
| `-t 1` (24 fps, 24 frames output) | 25 in 12 of 12 | 25 in 12 of 12 |
| to end of input | 48 in 12 of 12 | 48 in 12 of 12 |
| `trim=end_frame=20` on both inputs | 20 in 12 of 12 | 20 in 12 of 12 |
| reporter's 4K graph (QSV decode, `hwdownload`, `vmaf_4k_v0.6.1`, `-frames:v 200`), 3 runs | 201 201 201 | 201 201 201 |
| 4K QSV decode, `hwdownload`, `-frames:v 30`, 6 runs | 30 in 6 of 6 | 30 in 6 of 6 |

The reported CPU 200 came from a different pipeline than the SYCL run. Cause,
from fftools n9.0.2 (`ffmpeg_filter.c`, `ffmpeg_sched.c`, `ffmpeg_mux_init.c`,
`sync_queue.c`): `-frames:v` is enforced by the encoder sync queue and output
`-t` by the output trim, both after the filtergraph. The filter thread learns
that the output has closed only when `sch_filter_send` returns EOF or the
scheduler closes its inputs, asynchronously to the decoders that keep pushing
pairs. A `libvmaf*` filter scores as a side effect of filtering a pair, so it
scores pairs that `ffmpeg` then drops, and it has no way to know a pair will be
dropped. The race window depends on decoder and filter timing (rare at 4K with
QSV decode, frequent on small software-decoded input).

Options considered: document `trim=end_frame=N` on both inputs (chosen); a
frame-cap option on every `libvmaf*` filter (duplicates `trim`, another option
in five patches); patching fftools (out of the series' scope). On this branch
`trim` on QSV frames was checked: `trim=end_frame=20` on both QSV inputs scores
20 frames through zero-copy `libvmaf_sycl`.

## Validation on this branch

Arc A380 (i915, iHD), `localhost/vmafx:build-ocloc` (icpx 2026.1.1),
`scripts/test/sycl-dev-container.sh` with a fresh cache (icx C, SPIR-V JIT),
FFmpeg n9.0.2 with the series, on `52e265fc0` with the hybrid-toolchain branch
underneath, 2026-10-05:

| Check | Result |
| --- | --- |
| `--suite sycl` | 69 OK, 0 fail |
| `--suite fast` | 397 OK, 0 fail, 1 skipped (`test_sycl_ordered_sum` passes under `meson test`'s `MALLOC_PERTURB_`; it fails on master) |
| `zerocopy-e2e.sh --stage 1 --repeat 3 --depths 8` | `pass=50 fail=0 nonexact=0`: 18 stage-1 cases equal host upload in 3 of 3 runs, 32 stage-2/3 cases refused naming the extractor, host upload equal to the CPU everywhere (`ciede` 0 within its 1e-9 bound) |
| same, `--depths 10` | `pass=50 fail=0 nonexact=0` |
| `make sycl-zerocopy-contract` | routing check passes, comparator 26 of 26 |
| Patch series on `n9.0.2` | 20 of 20 apply with `git am --3way`; FFmpeg builds with `--enable-libvmaf-sycl` |
| `feature=name=psnr` on QSV input | `feature 'psnr' -> psnr_sycl`, refused at the first frame naming `psnr_sycl`, exit non-zero |
| `feature=name=niqe` on QSV / software input | configuration error `cannot run on zero-copy input: no SYCL twin` / warning and CPU fallback |
| `[vmaf-sycl] timing` line | absent at `-loglevel error`, present at `-loglevel info` and in the CLI |
| `trim=end_frame=20` on both QSV inputs | 20 frames scored |
