- The frame-synchronisation test exercises its synchronisation again. The
  pass that made `core/test/test_framesync.c` propagate teardown failures
  also dropped the one-second hold each worker takes after submitting its
  buffer, and that delay is the load the test exists to create: it is what
  keeps frame N's worker running while frame N+1's worker is already inside
  `vmaf_framesync_retrieve_filled_data()` waiting on frame N. Without it the
  workers ran to completion before either had to wait and the test passed in
  milliseconds having never entered the wait/signal path. The hold is
  restored (as `nanosleep()`, which POSIX specifies per-thread, rather than
  the `sleep()` it replaced — this runs on a thread-pool worker), and the
  test is back to its ~5 s runtime. The same pass had collapsed the eight
  per-step failure messages into one; each step now names itself again
  through a first-failure outcome that still lets teardown run
  unconditionally.
