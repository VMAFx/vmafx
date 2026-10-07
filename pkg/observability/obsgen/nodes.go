// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"github.com/grafana/grafana-foundation-sdk/go/dashboard"

	m "github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// NodesUID is the Nodes and devices dashboard's uid.
const NodesUID = "vmafx-nodes"

// backendMatcher filters node series by the dashboard's backend variable.
const backendMatcher = `backend=~"$backend"`

// nodes answers: which nodes run which backend, are they busy or idle, do
// their jobs fail, how long do jobs take, and how much device and host memory
// and CPU they use. The instance variable selects nodes.
func nodes() *dashboard.DashboardBuilder {
	b := newDashboard(NodesUID, "VMAFx Nodes and devices",
		"Worker nodes: backend and vendor, slot use, job outcomes and run time per node, GPU memory and host resources. GPU utilisation from a vendor exporter is in the exporter dashboards. Generated from pkg/observability/metricdef by tools/obsgen.",
		m.NodeInfo, scopeVariable("backend", "label_values("+m.NodeInfo.Name+`{job=~"$job"}, backend)`))
	return withRows(b, []row{
		{"Fleet", []panelBuilder{
			statPanel("Nodes by backend", "Nodes serving /metrics, per backend and GPU vendor.", unitCount, steps("blue"),
				instant("count by (backend, vendor) ("+fam(m.NodeInfo, backendMatcher)+")", "{{backend}} ({{vendor}})")).Span(8),
			statPanel("Slots in use", "Running controller jobs over configured slots across the selected nodes.", unitRatio,
				steps("green", at(0.8, "orange"), at(0.95, "red")),
				instant("sum("+fam(m.NodeJobsRunning)+") / clamp_min(sum("+fam(m.NodeSlots)+"), 1)", "slots in use")).Span(8),
			statPanel("Node job failures", "Share of controller jobs the selected nodes finished as failed over the window.", unitRatio,
				steps("green", at(0.01, "orange"), at(0.05, "red")),
				instant(rate(m.NodeJobs, "", backendMatcher, `outcome="failed"`)+" / clamp_min("+rate(m.NodeJobs, "", backendMatcher)+", 1e-9)", "failed")).Span(8),
		}},
		{"Jobs per node", []panelBuilder{
			timeseriesPanel("Running jobs and slots by node", "Jobs each node runs now against its slot count. A node at its slots is full; one at zero with a queue waiting cannot take the queued backend.", unitCount,
				query("sum by (instance) ("+fam(m.NodeJobsRunning)+")", "{{instance}} running"),
				query("sum by (instance) ("+fam(m.NodeSlots)+")", "{{instance}} slots")),
			timeseriesPanel("Jobs per minute by node and outcome", "Controller jobs each node finished per minute, by outcome.", unitPerMin,
				query(rate(m.NodeJobs, "instance, outcome", backendMatcher)+" * 60", "{{instance}} {{outcome}}")),
			timeseriesPanel("Job run time by backend", "Time nodes spent on one job, by backend: median and 95th percentile.", unitSeconds,
				query(quantile(0.5, m.NodeJobDuration, "backend", backendMatcher), "p50 {{backend}}"),
				query(quantile(0.95, m.NodeJobDuration, "backend", backendMatcher), "p95 {{backend}}")),
			timeseriesPanel("Job run time p95 by node", "95th percentile run time per node. One node far above the others of its backend points at that host or its GPU.", unitSeconds,
				query(quantile(0.95, m.NodeJobDuration, "instance", backendMatcher), "{{instance}}")),
		}},
		{"Devices and host", []panelBuilder{
			timeseriesPanel("GPU memory in use", "Device memory used per GPU, as a share of the device's memory (nvidia-smi on CUDA nodes, amdgpu sysfs on HIP nodes).", unitRatio,
				query("sum by (instance, device) ("+fam(m.NodeDeviceMemoryUsed)+") / clamp_min(sum by (instance, device) ("+fam(m.NodeDeviceMemoryTotal)+"), 1)", "{{instance}} {{device}}")),
			timeseriesPanel("Device memory reads failing", "Failed reads of the GPU memory per node and minute. A node reporting failures has no nvidia-smi or no readable sysfs files, and shows no GPU memory above.", unitPerMin,
				query(rate(m.MetricsReadErrors, "instance", `source="device_memory"`)+" * 60", "{{instance}}")),
			timeseriesPanel("Node process memory", "Resident memory of each node process.", "bytes",
				query("sum by (instance) ("+sel("process_resident_memory_bytes")+")", "{{instance}}")),
			timeseriesPanel("Node process CPU", "CPU seconds per second of each node process (1 = one core), the vmaf subprocesses excluded.", unitCount,
				query("sum by (instance) (rate("+sel("process_cpu_seconds_total")+rateWindow+"))", "{{instance}}")),
		}},
	})
}
