- **fix(controller)**: `PullWork` UPDATE now includes `AND status=?` with
  `StatusPending` — prevents a concurrent `Cancel` from being silently
  overwritten when it races with the job assignment (ADR-1012).
- **fix(controller)**: `ReportResult` UPDATE now includes `AND status NOT IN
  (completed,failed,cancelled)` — makes the call idempotent on gRPC retries
  and prevents a `Cancel` from being reinstated by a late retry (ADR-1012).
