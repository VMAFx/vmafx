# VMAFxQueueAging

**Meaning.** The oldest pending job of the tenant in the `tenant` label has
waited longer than `monitoring.alerts.queueAgeSeconds` (by default 1800
seconds, 30 minutes), and has done so for 15 minutes
(`vmafx_controller_queue_oldest_job_age_seconds`).

**Impact.** That tenant's work is late. Other tenants may be unaffected:
nodes only take jobs of their own tenant.

## Diagnose

1. Are there live nodes of that tenant? A node session belongs to one tenant
   (its token's `tid`). Overview _Live nodes_ shows all tenants; the node
   log's `RegisterNode` line shows which tenant a node serves.
2. Can a node run the job's backend? A job that names a backend (`cuda`,
   `sycl`, `hip`, `metal`, `cpu`) only goes to a node that advertises it.
   Compare the jobs' backend with `vmafx_node_info` on the Nodes and devices
   dashboard.
3. Are the nodes full? _Node slot use_ near 1 means every slot is busy: the
   queue grows faster than the nodes finish.

## Fix

Start nodes for that tenant and backend, or raise `VMAFX_NODE_SLOTS` on nodes
that have the capacity. If jobs ask for a backend no node offers, submit them
for an available backend or without one. The alert clears when the oldest
pending job is younger than the threshold.

Dashboards: Overview (_Oldest pending job_, _Queue wait_), Nodes and devices.
