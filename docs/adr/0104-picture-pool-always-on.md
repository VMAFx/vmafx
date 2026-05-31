<!-- markdownlint-disable MD013 MD060 -->
# ADR-0104: Compile `picture_pool` unconditionally and size it for the live-picture set

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: api, build, cli

## Context

The public headers [core/include/libvmaf/libvmaf.h](../../core/include/libvmaf/libvmaf.h)
declared `vmaf_preallocate_pictures` and `vmaf_fetch_preallocated_picture`
unconditionally, but the definitions in [core/src/libvmaf.c](../../core/src/libvmaf.c)
and [core/src/picture_pool.c](../../core/src/picture_pool.c) were behind
`#ifdef VMAF_PICTURE_POOL`. That flag was set by no default build, so any
external consumer linking against libvmaf.so got a link error on those symbols.
Issue #NN picked option **B** (always compile the pool) over **A** (gate the
declarations behind the same flag) so the public surface matches the shipped
library.

The initial always-on change in PR #32 hung the CPU `vmaf` CLI on frame 2 of
every run (Netflix golden gate, D24): `vmaf_picture_pool_fetch` blocked forever
in `pthread_cond_wait` because the pool was sized `(thread_cnt+1)*2` (or `2`
when `thread_cnt==0`) — one short of the live set. The internal
`vmaf->prev_ref` holds a reference to the previous frame's ref picture across
the frame boundary for motion features (see [libvmaf.c:978](../../core/src/libvmaf.c#L978))
so that slot is not returned until the *next* frame's prev_ref reassignment.

## Decision

We compile `picture_pool.c` unconditionally (removing the `-DVMAF_PICTURE_POOL`
meson gate and every `#ifdef VMAF_PICTURE_POOL` in the source tree), and size
the CPU CLI pool as `2 * (thread_cnt + 1) + 1`. The trailing `+ 1` accounts
for `vmaf->prev_ref` holding one pool picture permanently across frame
boundaries; the `2 * (thread_cnt + 1)` covers the CLI's currently-held
`(ref, dist)` pair plus one per worker thread's in-flight `(ref, dist)` pair.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| A. Gate header declarations behind `#ifdef VMAF_PICTURE_POOL` too | Smallest change; preserves opt-in semantics | Public ABI becomes build-flag-dependent; consumers can't call preallocation without rebuilding libvmaf | Defeats the point of a public API |
| B. Compile unconditionally, keep `pic_cnt = 2` | Trivial | Deadlocks on frame 2 (prev_ref holds one slot) | Caught by Netflix golden gate |
| C. Compile unconditionally, bump pic_cnt to cover prev_ref (**chosen**) | Fixes the hang; pool sizing matches liveness budget | Slightly more memory (~1–2 extra pictures) | — |
| D. Have `vmaf->prev_ref` hold a *copy* of the data, not a ref | Pool returns immediately | Defeats the point of the pool for motion; extra copy per frame | Performance regression |

## Consequences

- **Positive**: External consumers can call `vmaf_preallocate_pictures` /
  `vmaf_fetch_preallocated_picture` on any default build, closing the
  declared-but-undefined-symbol gap.
- **Positive**: CPU CLI no longer hangs on frame 2; Netflix golden gate (D24)
  passes deterministically.
- **Negative**: Minimum pool size grew by one picture per thread worker plus
  one for prev_ref. Extra allocation is bounded (pic_cnt linear in thread_cnt)
  and matches the existing live-picture set — no new copies, just earlier
  allocation.
- **Neutral / follow-ups**: test_pic_preallocation runs on every CPU build
  (was conditional on `-DVMAF_PICTURE_POOL`).

## References

- PR #32
- CLAUDE.md §8 (Netflix golden gate, non-override)
- [ADR-0101](0101-sycl-usm-picture-pool.md) (SYCL pool counterpart)
- Source: Q (popup): "Wire up pool-return on unref" — root-cause fix directive;
  diagnosis revealed the return path was already wired and the true root cause
  was pool sizing.
