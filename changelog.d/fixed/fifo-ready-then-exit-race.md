- **Python harness FIFO startup race**: in `fifo_mode`, a FIFO producer that
  signals readiness and returns immediately, such as `AssetExtractor`, is no
  longer reported intermittently as "child exited before signaling readiness".
  The startup wait now checks for child exit before it checks the readiness
  signal, so a producer that signalled before exiting is always accepted.
