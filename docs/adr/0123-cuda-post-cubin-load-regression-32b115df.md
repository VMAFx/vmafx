# ADR-0123: CUDA prev_ref null-deref on ffmpeg libvmaf_cuda path

- **Status**: Accepted
- **Date**: 2026-04-19
- **Deciders**: lusoris
- **Tags**: `cuda`, `regression`, `upstream-sync`

## Context

External user lawrence reported on 2026-04-19 that the lusoris fork
(latest `master`) segfaults through ffmpeg's `libvmaf_cuda` filter on
the first frame. Lawrence identified upstream commit
`32b115df92f04e715ad3efa1a66ae925dc69844d`
("libvmaf: add experimental `VMAF_BATCH_THREADING` and
`VMAF_PICTURE_POOL` threading modes", Kyle Swanson, 2026-04-07) as the
point where the issue entered both upstream Netflix/vmaf and the
fork.

ADR-0122 shipped defensive hardening (unconditional `sm_86` /
`sm_89` cubins + `compute_80` PTX fallback, actionable
`libcuda.so.1`-load error) but was explicitly not a fix for
lawrence's crash — a separate investigation (this ADR) was opened.

### Reproducer on a local RTX 4090 (`sm_89`)

1. Build libvmaf with CUDA enabled:

   ```bash
   meson setup core/build-cuda -Denable_cuda=true -Denable_sycl=false
   ninja -C core/build-cuda
   DESTDIR=/tmp/vmaf-install ninja -C core/build-cuda install
   ```

2. Build ffmpeg n8.1 against the uninstalled libvmaf, with
   `libvmaf_cuda`:

   ```bash
   PKG_CONFIG_PATH=/tmp/vmaf-install/usr/local/lib/pkgconfig \
   ./configure --enable-gpl --enable-nonfree --enable-libvmaf \
               --enable-cuda --enable-cuda-nvcc --enable-cuvid \
               --enable-nvdec --enable-nvenc --enable-ffnvcodec \
               --nvccflags="-gencode arch=compute_89,code=sm_89 -O2"
   make -j$(nproc)
   ```

3. Run `libvmaf_cuda` on an H.264 pair (e.g. Netflix golden YUV re-
   encoded with NVENC):

   ```bash
   ./ffmpeg -init_hw_device cuda=cu:0 -filter_hw_device cu \
     -i ref.mp4 -i dis.mp4 \
     -lavfi "[0:v]format=yuv420p,hwupload_cuda[r];\
             [1:v]format=yuv420p,hwupload_cuda[d];\
             [r][d]libvmaf_cuda=log_path=/tmp/out.json:log_fmt=json" \
     -f null -
   ```

   Exit 139 (SIGSEGV) on master at `aa6d7b4f`.

### Backtrace

Captured under `cuda-gdb`:

```text
Thread 41 "fc0" received signal SIGSEGV.
#0  vmaf_ref_fetch_increment ()          libvmaf.so.3
#1  vmaf_picture_ref ()                  libvmaf.so.3
#2  vmaf_read_pictures ()                libvmaf.so.3
#3  do_vmaf_cuda (fs=...)                vf_libvmaf.c:790
#4  ff_framesync_activate                libavfilter/framesync.c:364
```

### Root cause

[libvmaf.c:1428](../../core/src/libvmaf.c#L1428) does an
unguarded

```c
vmaf_picture_ref(&vmaf->prev_ref, ref);
```

at the non-threaded tail of `vmaf_read_pictures`. By then, the
`HAVE_CUDA` block at line 1403 has reassigned:

```c
ref  = &ref_host;
dist = &dist_host;
```

`ref_host` is a stack-local `VmafPicture = {0}`. It is populated by
`translate_picture_device` only when `hw_flags` contains
`HW_FLAG_HOST` — which is true only if at least one registered
extractor is not flagged `VMAF_FEATURE_EXTRACTOR_CUDA`. In the
ffmpeg `libvmaf_cuda` filter every registered extractor is CUDA, so
`rfe_hw_flags` returns `HW_FLAG_DEVICE` only,
`translate_picture_device` early-returns without downloading, and
`ref_host.ref` stays `NULL`. The subsequent
`vmaf_picture_ref` → `vmaf_ref_fetch_increment(NULL)` faults.

### Why the regression reached default builds

Three commits compose the bug:

- **Upstream `32b115df`** (2026-04-07) — introduced the experimental
  `VMAF_PICTURE_POOL` behind a meson gate. The new always-live
  `vmaf->prev_ref` slot was part of that work.
- **Upstream `f740276a`** (2026-04-09, "libvmaf: add support for
  `VMAF_FEATURE_EXTRACTOR_PREV_REF`") — moved the tail
  `vmaf_picture_ref(&vmaf->prev_ref, ref)` to an always-taken path
  in the non-threaded branch, with no guard against
  `ref->ref == NULL`.
- **Fork `65460e3a`** ([ADR-0104](0104-picture-pool-always-on.md),
  2026-04-18) — dropped the `VMAF_PICTURE_POOL` meson gate so the
  public-header symbols are always defined. Good for the library's
  ABI contract, but combined with the upstream pair it detonated the
  crash for every user of the `libvmaf_cuda` filter on default
  fork builds, which is how lawrence hit it.

The fork change was deliberate and performance-positive (user
confirmed +10 fps on the CPU path), so the remediation is to fix the
null-deref, not to re-gate the pool.

## Decision

Land a narrow null-guard at
[libvmaf.c:1428](../../core/src/libvmaf.c#L1428) that short-circuits
the `prev_ref` update when the selected `ref` has no refcount —
the situation that arises when all registered extractors are CUDA
and `ref_host` was therefore never populated:

```c
if (ref && ref->ref)
    vmaf_picture_ref(&vmaf->prev_ref, ref);
```

The only consumer of `VMAF_FEATURE_EXTRACTOR_PREV_REF` is the CPU
feature `integer_motion_v2`. A pure-CUDA extractor set (which is
what the ffmpeg filter registers) cannot have that flag in play, so
skipping the prev-ref update in this case is semantically correct,
not merely defensive.

SYCL is unaffected: `vmaf_read_pictures_sycl`
([libvmaf.c:1467](../../core/src/libvmaf.c#L1467)) does not
touch `vmaf->prev_ref` at all. The upstream fix still wants to land
upstream for the non-fork consumers.

## Alternatives considered

| Option | Pros | Cons |
| --- | --- | --- |
| Null-guard at the prev_ref update site (**chosen**) | Smallest diff; matches style of the loop's existing `&& vmaf->prev_ref.ref` guard; correct on the only affected code path. | Does not update `prev_ref` for CUDA-device-only setups — but there are no CUDA `PREV_REF` consumers, so this is a non-regression. |
| Select `ref_device` / `dist_device` on the CUDA-device-only path | Semantically feeds `prev_ref` with the same buffer type extractors consume. | Would silently place a CUDA picture into `prev_ref`; any future CPU extractor registering `PREV_REF` alongside CUDA ones would crash differently. Larger surface. |
| Revert fork commit `65460e3a` (re-gate the pool) | Closes the deref path by making the code never run on default builds. | Re-opens the declared-but-undefined symbol gap (issue #29) and loses the +10 fps CPU gain; user rejected this direction. |
| Fix upstream first, wait for Netflix merge | Cleanest long-term story. | Leaves lawrence and every other fork CUDA user broken until upstream review completes. |

## Consequences

- **Positive**: `libvmaf_cuda` filter runs end-to-end on `sm_89`
  (RTX 4090); full ffmpeg pipeline produces a valid JSON log.
  Reproducer above passes.
- **Positive**: All 32 meson tests pass. CPU CLI on the Netflix
  golden pair (`src01_hrc00` vs `src01_hrc01`) reports mean VMAF
  `76.667830` against the golden
  `76.66890519623612` — within `places=2`.
- **Positive**: The patch keeps the +10 fps CPU win from the
  always-on pool ([ADR-0104](0104-picture-pool-always-on.md)).
- **Neutral**: `vmaf->prev_ref` is not updated on the CUDA-device-
  only path. No currently-registered CUDA extractor reads it, so
  there is no behavioural change on that path.
- **Follow-up**: Port the null-guard upstream to Netflix/vmaf so
  non-fork ffmpeg users are unblocked too. The upstream tree also
  has the unguarded ref path (via `f740276a`); the experimental
  gate on `32b115df` happens to mask it for now.

## References

- ADR-0122 — defensive hardening (gencode + init) that preceded
  this investigation.
- [ADR-0104](0104-picture-pool-always-on.md) — fork's always-on
  picture pool decision.
- Upstream commit `f740276a` — adds the unguarded
  `vmaf_picture_ref(&vmaf->prev_ref, ref)` tail.
- Upstream commit `32b115df` — experimental
  `VMAF_PICTURE_POOL` / `VMAF_BATCH_THREADING` (lawrence's
  identified commit; prerequisite but not the deref site).
- Source: `req` — *paraphrased:* user confirmed null-guard is
  acceptable as the fix and that picture-pool must stay always-on
  because of the observed CPU throughput gain.
- External-user repro thread (2026-04-19, "lawrence"), including
  his `blackbeard-rocks/ffmpeg` `segfault` branch that documents
  the exact downstream build recipe.
