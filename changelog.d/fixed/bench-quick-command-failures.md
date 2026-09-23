- **The quick CLI benchmark no longer reports throughput for failed or hung
  VMAF processes.** Every external command now uses a resolved executable,
  closed stdin, captured diagnostics, and a finite timeout. The harness exits
  non-zero for command failures, timeouts, and an empty fixture set, and only
  benchmarks complete reference/distorted pairs.
