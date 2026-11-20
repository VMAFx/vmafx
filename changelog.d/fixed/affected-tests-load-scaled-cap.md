- **Tests:** the affected-tests runner applies a default 300 s per-test timeout
  when pytest actually loads pytest-timeout, and scales its 900 s base wall cap
  by `min(4, 1 + max(load1, load5) / CPUs)` to account for host load. Explicit
  registry timeouts fail if the plugin is missing or disabled; other suites
  report the missing protection and retain the unscaled base cap.
