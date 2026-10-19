# VMAFxNoLiveNodes

**Meaning.** The controller has pending jobs and no registered node for 10
minutes (`vmafx_controller_nodes_live` is 0 while
`vmafx_controller_jobs_pending` is above 0).

**Impact.** No submitted job runs. Callers waiting on job results time out.

## Diagnose

1. Are node pods running? `kubectl get pods -l app.kubernetes.io/component=node`.
2. Can they reach the controller? A node logs `controller client started` on
   success and `RegisterNode failed` with the reason on failure:
   `kubectl logs <node-pod> | grep -i register`. A refusal names the cause:
   a wrong `VMAFX_CONTROLLER_ADDR`, a token without the `vmafx:node` role, or
   TLS settings that do not match the controller.
3. Do they stay registered? A node that misses its heartbeats for 60 seconds
   is evicted; repeated `heartbeat failed` lines point at the network between
   node and controller.

## Fix

Start or fix the nodes, or correct their controller address, token or TLS
settings. Jobs start as soon as one node registers; the alert clears then.

Dashboards: Overview (_Live nodes_, _Pending jobs_), Nodes and devices.
