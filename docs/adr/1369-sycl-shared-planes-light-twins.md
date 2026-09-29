<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1369: SYCL twins read the planes the state uploads once per frame; opt-in shared chroma planes and a device-side slot fence

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: `sycl`, `gpu`, `performance`, `psnr`, `psnr-hvs`, `motion-v2`, `rc3`, `fork-local`

## Context

Every SYCL run uploads the luma of both pictures once per frame into the
double-buffered shared frame (`vmaf_sycl_shared_frame_upload`, called from
`read_pictures_sycl_prep` before any extractor submits). Several twins then
uploaded the same frame again on their own. Profiled at 3840x2160 on an Arc
B580 ([Research-1369](../research/1369-sycl-shared-planes-light-twins.md)):
`psnr_hvs_sycl` converted all three planes of both pictures to `float` on the
host (14.5 ms) and uploaded 99.5 MB (7.2 ms of DMA) for a 6.3 ms kernel;
`motion_v2_sycl` repacked and re-uploaded the reference luma; `psnr_sycl`
repacked and uploaded its own copy of the chroma, which psnr_hvs then uploaded
again. The maintainer's requirement is that planes do not make GPU-CPU round
trips ("there shouldnt be any gpu cpu rountrips"). The shared frame is
luma-only by design (`core/src/sycl/AGENTS.md`), because most runs (the
default model) never read chroma.

Uploading into a slot also had no ordering against the kernels that last read
it. The CLI's double-buffer dispatch waits for a frame's kernels in the next
frame's `collect()`, before the upload that reuses the slot, but an extractor
that skips a frame (`n_subsample`) has nothing collected, so a later upload
could overwrite a slot its kernels were still reading. Moving psnr_hvs onto
the shared slots would have extended that to it.

## Decision

We will make the SYCL state the single owner of per-frame plane uploads, and
have twins read those planes instead of uploading their own:

- **Opt-in shared chroma planes** in `core/src/sycl/common.cpp`:
  `vmaf_sycl_shared_chroma_init()` (idempotent per geometry, after
  `vmaf_sycl_shared_frame_init()`) allocates Cb / Cr of ref and dis for both
  slots; `vmaf_sycl_shared_chroma_upload()` puts the current frame's chroma
  into the compute slot once per frame — the first chroma-reading twin of a
  frame uploads, later ones return at once — by packing each plane into a
  pinned (host USM) staging buffer and issuing one DMA per plane on the copy
  queue, folded into `last_upload_event`.
  `vmaf_sycl_get_shared_plane()` returns a compute-slot plane (0 = luma).
  Luma-only runs never allocate or upload chroma.
- **`vmaf_sycl_queue_after_upload()`** gives a twin on its own queue the same
  device-side input barriers the combined graph gets (last upload, last
  de-tile), without a host wait.
- **A device-side slot fence** in `vmaf_sycl_shared_frame_upload()`: the
  upload into a slot waits on the device for markers
  (`ext_oneapi_get_last_event()` of the primary and combined queues) taken
  when that slot stopped being the compute slot; the copy-queue barrier is
  only submitted when a marker is still running.
- **psnr_sycl** reads chroma from the shared planes; **motion_v2_sycl** runs
  the [ADR-1371](1371-sycl-motion-diff-first-pipeline.md) SAD pipeline on the
  shared luma and lets its `cur_copy` keep the frame as the next frame's
  "prev"; **psnr_hvs_sycl** reads all three planes from the state as raw
  samples and scores every active plane in one dispatch, two work-items per
  8x8 block, with per-block float arithmetic unchanged.
- A psnr kernel rework that keeps the result bit-identical: one atomic per
  work-group instead of one per pixel, 32-bit squares. (A motion_v2 kernel
  that computed the vertical taps once per tile column in exact 32-bit
  arithmetic, 13.2 to 6.3 ms per 4K frame on the UHD 770, was dropped when
  ADR-1371 moved motion and motion_v2 onto one shared pipeline; it is a
  follow-up for that pipeline.)

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Shared chroma planes, uploaded lazily by the first twin that needs them (this ADR) | One upload per plane per frame for every twin; luma-only runs unchanged; no change to `libvmaf.c` or the extractor interface | Adds state and API to `common.cpp`; the zero-copy import path still has no chroma | Chosen |
| Copy chroma straight from the picture (one memcpy, or `ext_oneapi_memcpy2d` for pitched rows) | No packing copy | The pictures are pageable: the driver stages on the calling thread, and the pitched 2-D copy took 5.1 ms per 576x324 frame on the B580 and 168 ms on the UHD 770 | Measured, replaced by pinned staging |
| Always upload chroma with luma in `vmaf_sycl_shared_frame_upload` | Simplest ordering | Every SYCL run, the default model included, would pay 8.3 MB more DMA and 1 ms of host time per 4K frame for planes it never reads | Cost on the common path |
| Upload chroma in `read_pictures_sycl_prep` when an extractor asked for it | Upload before any submit | Extractors initialise lazily inside the first frame's dispatch, after `prep` has run, so frame 0 would need a second path anyway | More code for the same result |
| Keep per-twin uploads, only drop the host float conversion in psnr_hvs | Smallest diff | Still uploads luma twice or three times per frame when twins run together | Leaves the round trips the maintainer asked to remove |
| motion_v2 reads "prev" from the other shared slot instead of its own copy | Saves the 8 MB device-to-device copy | The next frame's upload overwrites that slot while this frame's kernel may still run (the hazard the fence addresses would become the normal case) | Correctness |
| Host-side fence (wait for readers before uploading) | Simple | Serialises upload after compute on the host, losing the upload/compute overlap the double buffer exists for | Throughput |
| Allocate the CLI's picture pool in host USM so uploads stop blocking the host | Removes the remaining 3-4 ms of host time per 4K frame (driver staging of pageable memory) | Changes the generic picture pool and `prepare_picture_pool`, which the CLI read-ahead work also touches | Follow-up, recorded in Research-1369 |
| A psnr_hvs kernel with a parallel per-block float order | Much faster on the UHD 770 | Per-block scores stop being bit-identical to the current twin (the ADR-1361 gate would still hold) | Bit-identity kept; open question in Research-1369 |

## Consequences

- **Positive**: at 3840x2160 on the B580 the per-frame cost psnr_hvs adds on
  top of the shared luma upload drops from about 29 ms (host conversion,
  private float upload, kernel) to about 3 ms (shared chroma upload and a
  1.3 ms kernel); motion_v2 and psnr lose their private uploads and host
  repacking (motion_v2 about 1.6 ms of host copy and DMA per 4K frame). Twins run together share one chroma upload. Scores are
  bit-identical to the previous twins on the Arc B580 and the UHD 770; psnr
  and motion_v2 equal the CPU, psnr_hvs keeps its ADR-1361 distance. psnr_hvs
  at 9 and 11 bits now reads the raw sample the CPU reads (it used to see 16
  times it). motion_v2 and luma-only psnr_hvs no longer dereference the NULL
  pictures the zero-copy import path passes to `submit()`.
- **Negative**: `common.cpp` owns more state (one aggregate member, the fence
  markers). On the zero-copy VA import path, which imports luma only, psnr and
  psnr_hvs with chroma now fail the frame with `-EINVAL` and an error log; they
  used to crash there. psnr_hvs remains slow on the UHD 770 (49 ms per 4K
  frame).
- **Neutral / follow-ups**: the pinned picture pool; other twins that stage
  chroma per extractor (`ciede_sycl`, `float_ssim_sycl` / `ssim_sycl`,
  `float_ms_ssim_sycl` with `enable_chroma`, `float_psnr_sycl`) can move onto
  the shared planes the same way; the CUDA and HIP twins of these three
  features carry the same overheads (RC3 rows in `docs/state.md`).
  `test_sycl_shared_planes` pins the helper contract and the three twins'
  per-frame CPU parity with changing chroma; `test_sycl_init_unwind` wraps the
  new init call.

## References

- Source: `req` — "benching = run it and find performance if possible, not
  just take a snapshot"; "there shouldnt be any gpu cpu rountrips".
- [Research-1369](../research/1369-sycl-shared-planes-light-twins.md) — the
  profile before and after, ablations, parity evidence, CUDA/HIP review.
- [ADR-1361](1361-psnr-hvs-area-scaled-parity-tolerance.md) — psnr_hvs gate;
  [ADR-0220](0220-sycl-fp64-fallback.md) — fp64-free kernels;
  [ADR-1121](1121-sycl-qsv-zerocopy-p010-normalization.md) — zero-copy import
  path; [ADR-0458](0458-sycl-cambi-ssim-slm-staging.md) — no per-step
  waits.
