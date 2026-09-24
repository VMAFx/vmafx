- HIP integer ADM now has a fast regression contract tying ADR-0759's four
  device-resident `AdmBufferHip` kernel parameters to their host launch,
  one-time upload and BUG-092 teardown lifecycle. This prevents a stale branch
  from silently restoring the 328-byte by-value kernel argument again. The
  device-free init-unwind harness also follows the current collector append
  seam, so it continues to execute its allocation-leak and error-result checks.
