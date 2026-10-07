// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"github.com/grafana/grafana-foundation-sdk/go/cog"
	"github.com/grafana/grafana-foundation-sdk/go/dashboard"

	m "github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// OverviewUID is the Overview dashboard's uid; the other dashboards link to it.
const OverviewUID = "vmafx-overview"

// overview answers, in this order: is every component up, is the queue
// keeping up, how much work finishes and how much fails, how long work waits
// and takes, what scores come out, and how busy the nodes are.
func overview() *dashboard.DashboardBuilder {
	b := newDashboard(OverviewUID, "VMAFx Overview",
		"Platform health at a glance: components, queue, throughput, latency, scores and node load. Generated from pkg/observability/metricdef by tools/obsgen.",
		tenantVariable())
	return withRows(b, []row{
		{"Health and queue", healthPanels()},
		{"Throughput", throughputPanels()},
		{"Latency", latencyPanels()},
		{"Scores and nodes", scoreAndNodePanels()},
	})
}

// panelBuilder is any panel the dashboard builder accepts.
type panelBuilder = cog.Builder[dashboard.Panel]

func healthPanels() []panelBuilder {
	tenant := tenantMatcher
	return []panelBuilder{
		statPanel("Components up", "Scrape targets of the selected jobs that answered the last scrape, per job. A job below its replica count has a target down.",
			unitCount, steps("red", at(1, "green")),
			instant(`sum by (job) (`+sel("up")+`)`, "{{job}}")),
		statPanel("Live nodes", "Nodes registered with the controller and sending heartbeats. Zero means queued jobs cannot run.",
			unitCount, steps("red", at(1, "green")),
			instant(orZero("max("+fam(m.ControllerNodesLive)+")"), "nodes")),
		statPanel("Pending jobs", "Jobs waiting in the controller queue for a node.",
			unitCount, steps("green"),
			instant(orZero("sum("+fam(m.ControllerJobsPending, tenant)+")"), "pending")),
		statPanel("Running jobs", "Jobs assigned to a node and not finished yet.",
			unitCount, steps("green"),
			instant(orZero("sum("+fam(m.ControllerJobsRunning, tenant)+")"), "running")),
		statPanel("Oldest pending job", "Age of the oldest job still waiting for a node. It grows when no node can take the work (none live, no matching backend, all slots busy).",
			unitSeconds, steps("green", at(300, "orange"), at(900, "red")),
			instant(orZero("max("+fam(m.ControllerQueueOldestAge, tenant)+")"), "oldest")),
		statPanel("Score request errors", "Share of synchronous Score requests (gRPC and POST /v1/score) that failed over the window.",
			unitRatio, steps("green", at(0.01, "orange"), at(0.05, "red")),
			instant(errorRatio(), "errors")),
	}
}

func errorRatio() string {
	return rate(m.ServerScoreErrors, "") + " / clamp_min(" + rate(m.ServerScoreRequests, "") + ", 1e-9)"
}

func throughputPanels() []panelBuilder {
	tenant := tenantMatcher
	return []panelBuilder{
		timeseriesPanel("Jobs per minute", "Jobs submitted to the queue and jobs reaching each terminal state, per minute.",
			unitPerMin,
			query(rate(m.ControllerJobsSubmitted, "", tenant)+" * 60", "submitted"),
			query(rate(m.ControllerJobsCompleted, "", tenant)+" * 60", "completed"),
			query(rate(m.ControllerJobsFailed, "", tenant)+" * 60", "failed"),
			query(rate(m.ControllerJobsCancelled, "", tenant)+" * 60", "cancelled")),
		timeseriesPanel("Job failure ratio by tenant", "Failed jobs over finished jobs (completed + failed), per tenant.",
			unitRatio,
			query(rate(m.ControllerJobsFailed, "tenant", tenant)+" / clamp_min("+
				rate(m.ControllerJobsFailed, "tenant", tenant)+" + "+rate(m.ControllerJobsCompleted, "tenant", tenant)+", 1e-9)", "{{tenant}}")),
		timeseriesPanel("Jobs returned to the queue", "Running jobs sent back to the queue, by reason: a lost node, a controller restart, or a failed assignment. Repeated node_lost points at unstable nodes.",
			unitCount,
			query(increase(m.ControllerJobsRequeued, "reason"), "{{reason}}")),
		timeseriesPanel("Score requests per second", "Synchronous Score requests (gRPC Score and POST /v1/score) and the ones that failed.",
			"reqps",
			query(rate(m.ServerScoreRequests, ""), "requests"),
			query(rate(m.ServerScoreErrors, ""), "errors")),
	}
}

func latencyPanels() []panelBuilder {
	tenant := tenantMatcher
	return []panelBuilder{
		timeseriesPanel("Queue wait", "Time from a job's submission until a node took it: median and 95th percentile.",
			unitSeconds,
			query(quantile(0.5, m.ControllerJobQueueWait, "", tenant), "p50"),
			query(quantile(0.95, m.ControllerJobQueueWait, "", tenant), "p95")),
		timeseriesPanel("Time to result", "Time from a job's submission until it completed: median and 95th percentile.",
			unitSeconds,
			query(quantile(0.5, m.ControllerJobDuration, "", tenant, `outcome="completed"`), "p50"),
			query(quantile(0.95, m.ControllerJobDuration, "", tenant, `outcome="completed"`), "p95")),
		timeseriesPanel("Score request latency", "Duration of synchronous Score requests: median and 99th percentile.",
			unitSeconds,
			query(quantile(0.5, m.ServerScoreDuration, ""), "p50"),
			query(quantile(0.99, m.ServerScoreDuration, ""), "p99")),
		timeseriesPanel("Node job run time", "Time nodes spent running one job, by backend: median and 95th percentile.",
			unitSeconds,
			query(quantile(0.5, m.NodeJobDuration, "backend"), "p50 {{backend}}"),
			query(quantile(0.95, m.NodeJobDuration, "backend"), "p95 {{backend}}")),
	}
}

func scoreAndNodePanels() []panelBuilder {
	tenant := tenantMatcher
	return []panelBuilder{
		timeseriesPanel("Median score by model", "Median pooled VMAF score of completed jobs and Score requests, per model. A drop without a change of content points at an encoder or pipeline regression.",
			unitScore,
			query(quantile(0.5, m.QualityScore, "model", tenant), "{{model}}")),
		timeseriesPanel("Node slot use", "Running jobs over configured slots across the selected nodes. Near 1 the nodes are the bottleneck; low with a growing queue, they cannot take the queued backend.",
			unitRatio,
			query("sum("+fam(m.NodeJobsRunning)+") / clamp_min(sum("+fam(m.NodeSlots)+"), 1)", "slots in use")),
	}
}
