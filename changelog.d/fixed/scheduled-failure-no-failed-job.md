- CI: the scheduled-failure reporter re-reads a run's jobs while the API has not listed the failed
  job yet, and exits 0 with a notice instead of failing when none appears; upstream-watcher run
  40515965410 had turned red a second time (`T-SCHEDULED-FAILURE-NO-FAILED-JOB-2026-10-27`).
