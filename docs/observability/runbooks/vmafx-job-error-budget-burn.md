# VMAFxJobErrorBudgetBurn

**Meaning.** Controller jobs fail faster than the objective
(`monitoring.slo.jobSuccess`, by default 99 % of jobs complete over 30 days)
allows. With the default settings the critical rule fires on a fast burn
(more than 14.4 % of the jobs of the last hour and of its last 5 minutes
failed), the warning rule on a slow one (more than 6 % over 6 hours and their
last 30 minutes); the alert's description states the values in force.
Recorded ratio: `vmafx:job_failure_ratio:rate<window>`.

**Impact.** Callers get failed jobs instead of scores; at this rate the
month's error budget runs out early.

## Diagnose

1. Which tenant? Overview _Job failure ratio by tenant_. One tenant alone
   points at its inputs (paths outside its scoring roots, unreadable remote
   sources, unknown models).
2. Which node or backend? Nodes and devices _Jobs per minute by node and
   outcome_ and _Node job failures_. One node alone points at that host (its
   GPU, its storage mounts, its vmaf binary).
3. What error? The node logs each failure with the job ID
   (`kubectl logs <node-pod> | grep -i "job" | grep -i error`); the job's
   `error` field (`GetJob`) carries the same text.

## Fix

Fix the cause the errors name: the tenant's inputs, the node, or a backend
the jobs cannot run on. A node can be drained by scaling it down; its running
jobs return to the queue. The alert clears when the short window falls below
the threshold.

Dashboards: Overview, Nodes and devices.
