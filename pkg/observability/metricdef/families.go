// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package metricdef

// Cardinality limits of the open labels. A process reports at most this many
// distinct values of the label; later values are merged under Overflow. The
// limits keep the largest family (vmafx_quality_score) below 11 000 series per
// process; docs/observability/metrics.md lists the bound of every family.
const (
	// TenantLimit bounds the tenant label.
	TenantLimit = 32
	// ModelLimit bounds the model label.
	ModelLimit = 16
)

// None is the value a label takes when the request or job carried no value
// for it: a request to vmafx-server, which has no tenants, or a job that named
// no backend.
const None = "none"

// The labels the families share.
var (
	// Tenant is the tenant that submitted the job or made the request.
	Tenant = Label{
		Name:  "tenant",
		Limit: TenantLimit,
		Doc:   "tenant of the job or request (the JWT `tid` claim); `none` on vmafx-server, which has no tenants",
	}
	// Model is the VMAF model the job or request named.
	Model = Label{
		Name:  "model",
		Limit: ModelLimit,
		Doc:   "VMAF model name the score came from; the default model when the request named none",
	}
	// Backend is the compute backend a job ran on.
	Backend = Label{
		Name:   "backend",
		Values: []string{"cpu", "cuda", "sycl", "hip", "metal", None},
		Doc:    "compute backend; `none` when the job named no backend and the node has none configured",
	}
	// Outcome is how a job ended.
	Outcome = Label{
		Name:   "outcome",
		Values: []string{"completed", "failed", "cancelled"},
		Doc:    "terminal state of the job",
	}
	// RequeueReason is why a running job went back to the queue.
	RequeueReason = Label{
		Name:   "reason",
		Values: []string{"node_lost", "controller_restart", "assign_rollback"},
		Doc:    "`node_lost`: the node missed its heartbeats and was evicted; `controller_restart`: the controller restarted with the job running; `assign_rollback`: an assignment failed after the job left the queue",
	}
	// Version is the build version of the binary.
	Version = Label{
		Name:  "version",
		Limit: 1,
		Doc:   "build version the binary reports with `--version`",
	}
	// Vendor is the GPU vendor behind a node's backend.
	Vendor = Label{
		Name:   "vendor",
		Values: []string{"nvidia", "amd", "intel", "apple", "cpu"},
		Doc:    "GPU vendor of the node's backend (`cuda` nvidia, `hip` amd, `sycl` intel, `metal` apple)",
	}
)

// Bucket layouts.
var (
	// RequestSecondsBuckets covers a synchronous scoring request, from a
	// rejected request to a long clip.
	RequestSecondsBuckets = []float64{0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, 30, 60, 120, 300, 600, 1800}
	// JobSecondsBuckets covers a queued job, from a short clip to two hours.
	JobSecondsBuckets = []float64{1, 5, 15, 30, 60, 120, 300, 600, 1200, 1800, 3600, 7200}
	// ScoreBuckets covers the VMAF scale, finer where most scores fall.
	ScoreBuckets = []float64{20, 30, 40, 50, 60, 70, 75, 80, 85, 88, 90, 92, 94, 96, 98, 100}
)

// BuildInfo identifies the build of every component that serves /metrics;
// the dashboards draw a deploy annotation where its version changes.
var BuildInfo = Family{
	Name: "vmafx_build_info", Kind: Gauge, Unit: "info",
	Help:     "Constant 1, labelled with the build version.",
	Labels:   []Label{Version},
	Emitters: []Component{Server, Controller, Node},
}

// Families of the synchronous scoring surface (Score and ScoreStream on gRPC,
// POST /v1/score on HTTP) of vmafx-server and vmafx-controller.
var (
	ServerScoreRequests = Family{
		Name: "vmafx_server_score_requests_total", Kind: Counter, Unit: "requests",
		Help:     "Total number of Score requests (HTTP + gRPC).",
		Emitters: []Component{Server, Controller},
	}
	ServerScoreErrors = Family{
		Name: "vmafx_server_score_errors_total", Kind: Counter, Unit: "requests",
		Help:     "Total number of Score requests that returned an error.",
		Emitters: []Component{Server, Controller},
	}
	ServerScoreDuration = Family{
		Name: "vmafx_server_score_duration_seconds", Kind: Histogram, Unit: "seconds",
		Help:     "End-to-end duration of a Score request in seconds.",
		Buckets:  RequestSecondsBuckets,
		Emitters: []Component{Server, Controller},
	}
	ServerHealthRequests = Family{
		Name: "vmafx_server_health_requests_total", Kind: Counter, Unit: "requests",
		Help:     "Total number of Health / healthz requests.",
		Emitters: []Component{Server, Controller},
	}
	ServerReadyRequests = Family{
		Name: "vmafx_server_ready_requests_total", Kind: Counter, Unit: "requests",
		Help:     "Total number of readyz requests.",
		Emitters: []Component{Server, Controller},
	}
)

// Families of the controller's job queue and node registry.
var (
	ControllerJobsSubmitted = Family{
		Name: "vmafx_controller_jobs_submitted_total", Kind: Counter, Unit: "jobs",
		Help:     "Total number of jobs submitted to the controller queue.",
		Labels:   []Label{Tenant},
		Emitters: []Component{Controller},
	}
	ControllerJobsCompleted = Family{
		Name: "vmafx_controller_jobs_completed_total", Kind: Counter, Unit: "jobs",
		Help:     "Total number of jobs that completed successfully.",
		Labels:   []Label{Tenant},
		Emitters: []Component{Controller},
	}
	ControllerJobsFailed = Family{
		Name: "vmafx_controller_jobs_failed_total", Kind: Counter, Unit: "jobs",
		Help:     "Total number of jobs that finished with an error.",
		Labels:   []Label{Tenant},
		Emitters: []Component{Controller},
	}
	ControllerJobsCancelled = Family{
		Name: "vmafx_controller_jobs_cancelled_total", Kind: Counter, Unit: "jobs",
		Help:     "Total number of jobs cancelled before they finished.",
		Labels:   []Label{Tenant},
		Emitters: []Component{Controller},
	}
	ControllerJobsRequeued = Family{
		Name: "vmafx_controller_jobs_requeued_total", Kind: Counter, Unit: "jobs",
		Help:     "Total number of running jobs returned to the queue since the controller started, by reason.",
		Labels:   []Label{RequeueReason},
		Emitters: []Component{Controller}, Scraped: true,
	}
	ControllerJobsPending = Family{
		Name: "vmafx_controller_jobs_pending", Kind: Gauge, Unit: "jobs",
		Help:     "Current number of PENDING jobs in the queue.",
		Labels:   []Label{Tenant},
		Emitters: []Component{Controller}, Scraped: true,
	}
	ControllerJobsRunning = Family{
		Name: "vmafx_controller_jobs_running", Kind: Gauge, Unit: "jobs",
		Help:     "Current number of RUNNING jobs in the queue.",
		Labels:   []Label{Tenant},
		Emitters: []Component{Controller}, Scraped: true,
	}
	ControllerQueueOldestAge = Family{
		Name: "vmafx_controller_queue_oldest_job_age_seconds", Kind: Gauge, Unit: "seconds",
		Help:     "Age of the oldest PENDING job in the queue; absent while the tenant has none.",
		Labels:   []Label{Tenant},
		Emitters: []Component{Controller}, Scraped: true, MergeMax: true,
	}
	ControllerNodesLive = Family{
		Name: "vmafx_controller_nodes_live", Kind: Gauge, Unit: "nodes",
		Help:     "Current number of registered (live) vmafx-node instances.",
		Emitters: []Component{Controller}, Scraped: true,
	}
	ControllerJobQueueWait = Family{
		Name: "vmafx_controller_job_queue_wait_seconds", Kind: Histogram, Unit: "seconds",
		Help:     "Time a job waited in the queue, from submission to its assignment to a node.",
		Labels:   []Label{Tenant},
		Buckets:  JobSecondsBuckets,
		Emitters: []Component{Controller},
	}
	ControllerJobDuration = Family{
		Name: "vmafx_controller_job_duration_seconds", Kind: Histogram, Unit: "seconds",
		Help:     "Time from a job's submission to its terminal state.",
		Labels:   []Label{Tenant, Outcome},
		Buckets:  JobSecondsBuckets,
		Emitters: []Component{Controller},
	}
)

// QualityScore is the quality family: the distribution of the scores the
// platform produced, per tenant and model.
var QualityScore = Family{
	Name: "vmafx_quality_score", Kind: Histogram, Unit: "score",
	Help:     "Distribution of the pooled VMAF scores of completed jobs and Score requests.",
	Labels:   []Label{Tenant, Model},
	Buckets:  ScoreBuckets,
	Emitters: []Component{Server, Controller},
}

// Families of the worker node.
var (
	NodeInfo = Family{
		Name: "vmafx_node_info", Kind: Gauge, Unit: "info",
		Help:     "Constant 1, labelled with the backend the node runs (VMAFX_BACKEND) and its GPU vendor.",
		Labels:   []Label{Backend, Vendor},
		Emitters: []Component{Node},
	}
	NodeSlots = Family{
		Name: "vmafx_node_slots", Kind: Gauge, Unit: "slots",
		Help:     "Number of controller jobs the node runs at once (VMAFX_NODE_SLOTS); 0 without a controller.",
		Emitters: []Component{Node},
	}
	NodeJobsRunning = Family{
		Name: "vmafx_node_jobs_running", Kind: Gauge, Unit: "jobs",
		Help:     "Number of controller jobs the node is running now.",
		Emitters: []Component{Node},
	}
	NodeJobs = Family{
		Name: "vmafx_node_jobs_total", Kind: Counter, Unit: "jobs",
		Help:     "Total number of controller jobs the node finished, by backend and outcome.",
		Labels:   []Label{Backend, Outcome},
		Emitters: []Component{Node},
	}
	NodeJobDuration = Family{
		Name: "vmafx_node_job_duration_seconds", Kind: Histogram, Unit: "seconds",
		Help:     "Time the node spent running one controller job.",
		Labels:   []Label{Backend},
		Buckets:  JobSecondsBuckets,
		Emitters: []Component{Node},
	}
)

// All returns every family, in the order of the reference page.
func All() []Family {
	return []Family{
		BuildInfo,
		ServerScoreRequests, ServerScoreErrors, ServerScoreDuration,
		ServerHealthRequests, ServerReadyRequests,
		ControllerJobsSubmitted, ControllerJobsCompleted, ControllerJobsFailed,
		ControllerJobsCancelled, ControllerJobsRequeued,
		ControllerJobsPending, ControllerJobsRunning, ControllerQueueOldestAge,
		ControllerNodesLive, ControllerJobQueueWait, ControllerJobDuration,
		QualityScore,
		NodeInfo, NodeSlots, NodeJobsRunning, NodeJobs, NodeJobDuration,
	}
}
