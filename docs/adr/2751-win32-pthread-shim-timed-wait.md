<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2751: MSVC threads stay on the Win32 shim, which gains a timed wait

- **Status**: Accepted
- **Date**: 2026-10-08
- **Deciders**: lusoris
- **Tags**: build, windows, threads, upstream-port

## Context

Upstream Netflix/vmaf `bcd6e6159` ("MSVC: Add Windows pthreads support
(bundled + external)") gives MSVC builds a threads library: the
GerHobbelt/pthread-win32 fork as a git submodule built through a CMake
subproject, a `-Dbundled_winpthreads` option, and a search for an installed
pthreadVC3. The fork has built with MSVC since ADR-0121 on a header-only shim,
`core/src/compat/win32/pthread.h`, which maps the calls libvmaf makes onto
SRWLOCK, CONDITION_VARIABLE, INIT_ONCE and `_beginthreadex`.

Every pthread function the library and the tools call is in the shim
(`core/test/test_win32_pthread_shim_contract.py` keeps it that way). Nothing
uses mutex attributes, read-write locks, barriers, thread-local keys,
cancellation, priorities or stack sizes. One gap was real: the shim had no
`pthread_cond_timedwait()`. As a result `test_thread_pool_backpressure` did not
build on MSVC, and the VMAFx host fences polled with a 1 ms sleep on Windows
(50 µs elsewhere) instead of sleeping until signalled.

The pthread-win32 sources at upstream's pin carry LGPL-2.0-or-later headers;
the library would link them statically into `vmaf.lib`. Their mutex is a heap
object with a kernel event, lazily created under a global lock for the static
initializer, and their condition variable is built on semaphores and an
internal mutex. SRWLOCK and CONDITION_VARIABLE are one pointer each and stay in
user mode until there is contention.

## Decision

We keep the Win32 shim as the only threads implementation of MSVC, clang-cl
and icx-cl builds, and do not take `bcd6e6159` or the pthread-win32 parts of
the upstream MSVC series. The shim gains `pthread_cond_timedwait()`: the
absolute `CLOCK_REALTIME` deadline is turned into a relative timeout, rounded
up to milliseconds and capped below `INFINITE` (`vmaf_w32_timeout_ms()` in
`core/src/compat/win32/pthread_timeout.h`, tested on every host). A wake
before the deadline returns 0, and `ETIMEDOUT` is returned only once the clock
has passed the deadline. The VMAFx host fence waits on a condition variable
that its signal broadcasts, in chunks of at most 1 s against a monotonic
deadline, on every platform; on Linux the condition variable measures
`CLOCK_MONOTONIC`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the shim, add the timed wait (chosen) | One implementation; no new dependency or licence; lighter primitives; host fences stop polling | The fork owns the shim | — |
| Take upstream's bundled pthread-win32 | Full POSIX threads surface on MSVC; matches upstream's build | Second threads implementation (HISS-19); git submodule plus CMake subproject in a Meson build; three library flavours to rename for consumers; LGPL headers linked statically; heavier mutex and condition variable | Nothing the tree calls is missing from the shim apart from the timed wait |
| Keep the shim without a timed wait | No change | `test_thread_pool_backpressure` stays off the MSVC lanes; host fences keep polling | The gap was the one real difference |
| Timed host-fence waits on POSIX only, polling on Windows | No Windows change | Two wait paths, and Windows keeps the 1 ms poll | The maintainer asked for the timed wait on Windows |

## Consequences

- **Positive**: the MSVC lanes build and run `test_thread_pool_backpressure`
  and `test_vmafx_host_fence_wait`; host fences wake on the signal instead of
  at the next poll, on every platform.
- **Negative**: the shim's timed wait is proven on Windows only by the hosted
  MSVC legs; on Linux its arithmetic is unit-tested and the call itself was
  exercised under Wine during development.
- **Neutral / follow-ups**: a new pthread call in code that builds on Windows
  needs a shim function in the same pull request (the contract test fails
  otherwise).

## References

- Q-302 (maintainer, 2026-10-08), the question: "what is better? ours sounds
  more efficient — do we miss any features?"; the decision: "keep our shim;
  bcd6e6159 = NOT-APPLICABLE with your reasons (efficiency, no LGPL static
  link, no second implementation). Add pthread_cond_timedwait to
  core/src/compat/win32/pthread.h (SleepConditionVariableSRW, correct
  absolute-to-relative timeout conversion, ETIMEDOUT mapping, loop-safe) in a
  small separate PR: switch the device-frames host fence from 1 ms polling to
  the timed wait on Windows and un-skip test_thread_pool_backpressure on MSVC
  (has_cond_timedwait); extend test_win32_pthread_shim_contract.py".
- [ADR-0121](0121-windows-gpu-build-only-legs.md) — the shim's origin.
- [ADR-1929](1929-vmafx-device-frames-fences.md) — host fences.
- Upstream Netflix/vmaf `bcd6e6159`, `a2660554e`, `8618ba5cd`, `5079124ea`,
  `e2f8b24ab`.
