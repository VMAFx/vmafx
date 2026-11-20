- **Tests:** the affected-tests runner applies a default 300 s per-test timeout
  when pytest-timeout is installed, and scales its 900 s base wall cap by
  `min(4, 1 + max(load1, load5) / CPUs)` to account for host load.
