- Fixed FIFO-mode Python executors hanging until the CI job timeout when a
  spawned workfile or procfile producer died before signaling readiness. The
  parent now reports the producer role, exit code, and available traceback,
  retains the five-second slow-start warning, and fails after a bounded
  60-second startup window (BUG-090).
