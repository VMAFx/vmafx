- **Controller job metrics are per tenant (RC4, ADR-2349).**
  `vmafx_controller_jobs_submitted_total`, `_completed_total`,
  `_failed_total`, `vmafx_controller_jobs_pending` and `_jobs_running` carry
  a `tenant` label, so a query on the bare series returns one series per
  tenant: wrap it in `sum()` for the total. A repeated result report of a
  finished job is no longer counted again, and `vmafx_server_score_duration_seconds`
  buckets reach 30 minutes (0.05 s to 1800 s) so long clips land in a bucket.
  `vmafx-server` no longer serves the controller's job counters, which it
  never incremented.
