<!-- markdownlint-disable MD013 MD060 -->
# Dashboard tour

VMAFx ships ten Grafana dashboards, generated from the same metric definition
the services use (`deploy/grafana/dashboards/`; the Helm chart and the Compose
example install them). This page says what each one is for and how to read
it. Every dashboard has the same three variables at the top:
**Prometheus data source**, **Job** and **Instance** (the scrape jobs and
targets that serve `vmafx_build_info`; _All_ selects every one). Dashboards
about tenants add **Tenant**, the tenants that submitted jobs. The
_Deploys and restarts_ annotation marks each component start with its version,
so a step in a graph can be read against a rollout. The links at the top right
open the other VMAFx dashboards with the same time range and variables.

| Dashboard | Start here when |
| --- | --- |
| [Overview](#overview) | you want to know whether VMAFx works right now |
| [Quality](#quality) | scores moved, or a tenant reports worse quality |
| [Nodes and devices](#nodes-and-devices) | jobs are slow, fail on some nodes, or the fleet looks full |
| [Live sessions](#live-sessions) | streaming (ScoreStream) clients report problems |
| [SLO report](#slo-report) | you report reliability, or a burn-rate alert fired |
| [Usage and cost](#usage-and-cost) | you account usage per tenant |
| [Capacity](#capacity) | you plan nodes, or the queue keeps growing |
| [GPU exporter dashboards](#gpu-exporter-dashboards) | you need GPU utilisation, encoder load or power |

## Overview

`vmafx-overview.json` answers, row by row:

| Row | Panels | Question |
|-----|--------|----------|
| Health and queue | Components up, Live nodes, Pending jobs, Running jobs, Oldest pending job, Score request errors | Is every component scraped, can queued work run, is the queue draining? |
| Throughput | Jobs per minute, Job failure ratio by tenant, Jobs returned to the queue, Score requests per second | How much work finishes, and how much fails or is retried? |
| Latency | Queue wait, Time to result, Score request latency, Node job run time | How long does work wait and take? |
| Scores and nodes | Median score by model, Node slot use | Did the scores move, and are the nodes the bottleneck? |

A growing _Oldest pending job_ with _Live nodes_ above zero and low _Node
slot use_ means no node can take the queued work: compare the jobs' backend
with the nodes' (`vmafx_node_info`).

When something looks wrong: _Components up_ below the number of pods is
[component down](runbooks/vmafx-component-down.md); pending jobs with no live
node is [no live nodes](runbooks/vmafx-no-live-nodes.md); an old pending job
is [queue aging](runbooks/vmafx-queue-aging.md).

## Quality

`vmafx-quality.json` follows the scores the platform produced. Its **Tenant**,
**Model** and **Profile** variables narrow the selection.

| Row | Panels | Question |
|-----|--------|----------|
| Score levels | Median score by model, 10th percentile score by model, Median score by tenant, Share of scores below 70 | Did the scores move, for everyone or for one tenant or model, and how bad is the tail? |
| Distribution and volume | Score distribution (heatmap), Scores per minute by model | Did the whole distribution shift or did a second band appear, and how much work is behind the numbers? |

A drop on one model only after a model change points at the model; a drop on
one tenant only points at its content or encodes.

The alert behind this dashboard is
[score regression](runbooks/vmafx-score-regression.md).

## Nodes and devices

`vmafx-nodes.json` covers the worker nodes; its **Job** and **Instance**
variables list the node targets (series `vmafx_node_info`), **Backend** the
backends.

| Row | Panels | Question |
|-----|--------|----------|
| Fleet | Nodes by backend, Slots in use, Node job failures | What runs where, is the fleet full, do jobs fail? |
| Jobs per node | Running jobs and slots by node, Jobs per minute by node and outcome, Job run time by backend, Job run time p95 by node | Which node is busy, idle, failing or slow? |
| Devices and host | GPU memory in use, Device memory reads failing, Node process memory, Node process CPU | Is a GPU out of memory, and is the node process itself the limit? |

When something looks wrong: GPU memory missing on a GPU node, with _Device
memory reads failing_ above zero, is
[metrics read errors](runbooks/vmafx-metrics-read-errors.md).

## Live sessions

`vmafx-live.json` covers ScoreStream, the per-frame streaming path of
`vmafx-server` and `vmafx-node` (ADR-0933).

| Row | Panels | Question |
|-----|--------|----------|
| Sessions now | Open sessions, Frames per second, Failed sessions, Median session length | How much live scoring runs now, and does it fail? |
| Over time | Open sessions by instance, Frames per second by instance, Sessions ended per minute by outcome, Session duration | Do sessions spread over the instances, and how do they end? |

A session the client stops (cancelled call or passed deadline) counts as
`cancelled`, not `failed`.

## SLO report

`vmafx-slo.json` reports each SLO over the dashboard's time range, 30 days by
default. The objectives are the rule settings (`monitoring.slo`), which the
rules record as `vmafx:slo_objective{slo}`; the events are recorded every 5
minutes per SLO as `vmafx:slo_events:rate5m` and
`vmafx:slo_bad_events:rate5m`, and the report adds them up over its window,
so every 5 minutes count by their traffic.

| Row (one per SLO: Jobs, Score errors, Score latency) | Panels | Question |
|-----|--------|----------|
| Compliance | Compliance, Objective, Error budget left, Burn rate over the last hour | Is the objective met over the window, how much budget is left, and how fast is it spent now? |
| Bad events against the budget | Hourly bad-event ratio and the budget line | When was the budget spent? |

A burn rate of 1 spends the budget exactly over 30 days; the fast burn-rate
alert fires at 14.4 by default.

The burn-rate alerts behind this report:
[job error budget](runbooks/vmafx-job-error-budget-burn.md),
[Score error budget](runbooks/vmafx-score-error-budget-burn.md) and
[Score latency budget](runbooks/vmafx-score-latency-budget-burn.md). The
objectives are set in `monitoring.slo` ([Kubernetes](kubernetes.md#set-the-objectives-and-thresholds),
[Compose](compose.md#change-the-alert-settings)).

## Usage and cost

`vmafx-usage.json` counts what each tenant used over the time range, 7 days
by default, and prices it (decision Q-193):

| Row | Panels | Question |
|-----|--------|----------|
| Usage over the time range | Finished jobs by outcome, Job run time, Scores, by tenant | What did each tenant use? |
| Cost over the time range | Run-time cost, Per-job cost, Total cost, by tenant | What did that cost at the configured prices? |
| Usage over time | Node time per hour, Jobs per hour, by tenant | When did the tenant use it? |

The cost model is the rule settings' `monitoring.cost`: a price per second of
job run time (`perJobSecond`) and one per job (`perJob`), the same for every
backend, in `currency` (decisions Q-193, Q-208). The per-job price applies to
completed and failed jobs; cancelled jobs are not charged. The run time of
every finished job counts, whatever its outcome; it is the controller's
`vmafx_controller_job_duration_seconds`. A promtool case in
`deploy/prometheus/vmafx-rules.test.yaml` evaluates the per-job cost query: a
cancelled job adds no cost. The
prices are recorded as `vmafx:price_job_second` and `vmafx:price_job`, only
while they are above 0: an unset price leaves its cost panels empty, and no
price is ever assumed.

Set the prices in `monitoring.cost` ([Kubernetes](kubernetes.md#set-the-objectives-and-thresholds),
[Compose](compose.md#change-the-alert-settings)); they are yours to choose.

## Capacity

`vmafx-capacity.json` sets the nodes' measured capacity against the demand:

| Row | Panels | Question |
|-----|--------|----------|
| Now | Measured capacity, Demand, Headroom, Demand growth per day | How many jobs per hour can the nodes finish, how many arrive, and how fast does demand grow? |
| Forecast | Pending jobs with a 24-hour forecast; demand with a 7-day forecast against capacity | Where are the queue and the demand heading? |

Capacity is the nodes' slots times 3600 over the mean job run time of the last
6 hours (`vmafx_node_job_duration_seconds`), demand the jobs submitted in the
last hour; the forecasts are `predict_linear` over the last 6 hours (queue)
and the last day (demand), the growth `deriv` over the last day.

Headroom below zero means the queue grows: the
[queue aging](runbooks/vmafx-queue-aging.md) alert follows. Add nodes or
slots (`VMAFX_NODE_SLOTS`) for the backend the jobs ask for.

## GPU exporter dashboards

GPU utilisation, encoder and decoder load and power come from the vendors'
exporters, not from VMAFx. One dashboard per exporter is generated, each with
its own **Job** and **Instance** variables over the exporter's targets:

| File | Exporter | Series |
|------|----------|--------|
| `vmafx-gpu-dcgm.json` | NVIDIA dcgm-exporter | `DCGM_FI_DEV_GPU_UTIL`, `DCGM_FI_DEV_FB_USED`, `DCGM_FI_DEV_FB_FREE`, `DCGM_FI_DEV_ENC_UTIL`, `DCGM_FI_DEV_DEC_UTIL`, `DCGM_FI_DEV_POWER_USAGE` |
| `vmafx-gpu-amd.json` | AMD device-metrics-exporter | `gpu_gfx_activity`, `gpu_used_vram`, `gpu_total_vram`, `gpu_package_power` |
| `vmafx-gpu-intel.json` | Intel XPU Manager | `xpum_engine_ratio`, `xpum_memory_ratio`, `xpum_engine_group_ratio`, `xpum_power_watts` |

Import one only where its exporter runs. The contract test allows an
exporter's series only on the dashboard tagged `vmafx-exporter-<name>`, so no
VMAFx dashboard shows "No data" on a cluster without that exporter.

## See also

- [Alert runbooks](runbooks/index.md): what to do when an alert fires.
- [Metric reference](metrics.md): every series the dashboards read.
- [Monitoring on Kubernetes](kubernetes.md) and
  [the Compose stack](compose.md): installing the dashboards.
- [Observability (developer guide)](../development/observability.md#dashboards):
  how the dashboards are generated and checked.
