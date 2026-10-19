# VMAFxComponentDown

**Meaning.** Prometheus has failed to scrape a VMAFx target (a server,
controller or node `/metrics` page) for 5 minutes, and that target served
`vmafx_build_info` within the last hour. The labels name the scrape `job` and
the `instance`.

**Impact.** A down controller stops the queue: no job is submitted,
assigned or reported. A down node runs no jobs; its running jobs return to
the queue once the controller evicts it (`vmafx_controller_jobs_requeued_total{reason="node_lost"}`).
A down server refuses synchronous scoring. Everything that target reported
is missing from the dashboards meanwhile.

## Diagnose

1. Is the process running? In Kubernetes:
   `kubectl get pods -l app.kubernetes.io/component=<server|controller|node>`
   and `kubectl describe pod <pod>` for restarts, OOM kills or pending
   scheduling.
2. If it runs, is the page reachable? From inside the cluster:
   `curl -s http://<pod-ip>:<port>/metrics | head` (server and controller on
   `VMAFX_HTTP_ADDR`, default `:8080`; node on `:9090`). A connection refusal
   means the listener is not up: read the startup log
   (`kubectl logs <pod>`), which names the configuration error.
3. If the page answers but Prometheus still marks the target down, compare
   the target's address in the Prometheus targets page with the pod and its
   ServiceMonitor or PodMonitor port.

## Fix

Restore the process (fix the configuration or resources the log names, or
reschedule the pod). For a scrape-only problem, fix the monitor's port or
path. The alert clears on the next successful scrape. A target that is gone
for good stops alerting an hour after its last `vmafx_build_info`.

Dashboard: Overview, panel _Components up_.
