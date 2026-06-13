**queue**: fix `PullWork` orphaning jobs in `RUNNING` state when the
post-UPDATE `getUnlocked` read fails (round-25 audit B.1, ADR-0961).

Previously, if `getUnlocked` failed after the SQL `UPDATE jobs SET
status='running'` committed, the job was stranded in `RUNNING` with no node
assignment and no FIFO entry — recoverable only via controller restart.  The
fix adds an explicit rollback: resets SQL status to `pending`
(`assigned_node=NULL`), removes the entry from `runningSet`, and re-prepends
the FIFO entry so the job is retried on the next `PullWork` call.  A
`CRITICAL`-level log is emitted when the rollback itself fails.
