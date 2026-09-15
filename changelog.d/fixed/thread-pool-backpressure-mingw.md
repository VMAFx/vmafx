- **The thread-pool backpressure test builds on both Windows toolchains again.**
  It bounds every wait with `pthread_cond_timedwait` so a broken gate fails
  instead of hanging the suite, and took its deadline from
  `timespec_get(&deadline, TIME_UTC)`. MinGW-w64's `<time.h>` does not declare
  `timespec_get`, so the required `Windows MinGW64` lane failed to build. The
  deadline now comes from `clock_gettime(CLOCK_REALTIME, &deadline)`, which is
  what `core/test/test_fex_pool_growth.c` already uses and is the clock
  `pthread_cond_timedwait` measures against.

  That exposed the larger problem on the other Windows lane: MSVC has neither
  `clock_gettime` nor `CLOCK_REALTIME`, and — clock aside — no
  `pthread_cond_timedwait` either. Nor does this repo's Win32 shim
  (`core/src/compat/win32/pthread.h` implements init, destroy, wait, signal and
  broadcast over `CONDITION_VARIABLE`, but no timed wait), so on `Windows MSVC +
  CUDA` the call had only ever been an implicit declaration. The target is now
  built only where `pthread_cond_timedwait` actually exists, probed with
  `cc.has_header_symbol` rather than matched on a platform name, so it comes
  back by itself if the shim ever grows one.
