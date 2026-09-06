// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/observability/otel_instruments.go — canonical OTel span names, metric
// instrument names, and attribute keys for VMAFX.
//
// All four binaries (vmafx-controller, vmafx-node, vmafx-server, vmafx-mcp)
// import this file so names and keys are defined once.
//
// Cardinality budget (ADR-0782):
//   - Span attributes: job_id, model, backend, node_id — all bounded.
//   - Metric label sets: {backend}, {model}, {vendor} — at most ~10 values each.
//   - No per-file / per-clip high-cardinality labels on metrics.
//
// ADR-0782: OpenTelemetry tracing and metrics schema.

package observability

import (
	"context"
	"time"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/codes"
	"go.opentelemetry.io/otel/metric"
	"go.opentelemetry.io/otel/trace"
)

// ---------------------------------------------------------------------------
// Span names
// ---------------------------------------------------------------------------

const (
	// SpanJobSubmit traces SubmitJob gRPC handler on vmafx-controller.
	SpanJobSubmit = "vmafx.job.submit"
	// SpanEncoderDispatch traces encoder selection + ffmpeg invocation on vmafx-node.
	SpanEncoderDispatch = "vmafx.encoder.dispatch"
	// SpanFrameExtraction traces per-frame feature extraction (libvmaf call).
	SpanFrameExtraction = "vmafx.frame.extraction"
	// SpanScoring traces the end-to-end VMAF scoring pipeline.
	SpanScoring = "vmafx.scoring"
	// SpanONNXInference traces ONNX Runtime inference: in-process on
	// vmafx-node, and the vmafx-ort-runner subprocess call in pkg/ai (the
	// runner itself is OTel-exempt, ADR-1134 — its caller owns the span).
	SpanONNXInference = "vmafx.onnx.inference"
	// SpanMCPTool traces one MCP tool call on vmafx-mcp (stdio or HTTP
	// transport); the tool name rides on AttrMCPTool.
	SpanMCPTool = "vmafx.mcp.tool"
	// SpanTuneCommand traces one vmafx-tune subcommand invocation end to end;
	// the cobra command path rides on AttrTuneCommand.
	SpanTuneCommand = "vmafx.tune.command"
)

// ---------------------------------------------------------------------------
// Attribute keys
// ---------------------------------------------------------------------------

var (
	AttrJobID   = attribute.Key("vmafx.job_id")
	AttrModel   = attribute.Key("vmafx.model")
	AttrBackend = attribute.Key("vmafx.backend")
	AttrNodeID  = attribute.Key("vmafx.node_id")
	AttrVendor  = attribute.Key("vmafx.gpu_vendor")
	AttrStatus  = attribute.Key("vmafx.status") // "ok" | "error"
	// AttrMCPTool is the MCP tool name on SpanMCPTool (bounded: the tool
	// list in cmd/vmafx-mcp/tools.go, ~15 values).
	AttrMCPTool = attribute.Key("vmafx.mcp.tool")
	// AttrTuneCommand is the cobra command path on SpanTuneCommand (bounded:
	// the subcommand tree in cmd/vmafx-tune/cmd/root.go, ~20 values).
	AttrTuneCommand = attribute.Key("vmafx.tune.command")
)

// ---------------------------------------------------------------------------
// OTelMetrics — OTel-native metric instruments.
// These complement (not replace) the existing Prometheus Metrics struct.
// ---------------------------------------------------------------------------

// OTelMetrics holds the OTel SDK instruments registered by InitOTelMetrics.
type OTelMetrics struct {
	// JobsQueued is the current depth of the controller job queue (UpDownCounter).
	JobsQueued metric.Int64UpDownCounter
	// JobsInFlight is the number of jobs currently assigned to a node.
	JobsInFlight metric.Int64UpDownCounter
	// FramesPerSecond is a histogram of frame-extraction throughput (fps).
	FramesPerSecond metric.Float64Histogram
	// ScoreLatency is a histogram of end-to-end scoring latency in ms.
	// p50/p99 are derived from the bucket series by OTLP consumers.
	ScoreLatency metric.Float64Histogram
	// GPUUtilization is a gauge of GPU compute utilisation (0–100 %).
	GPUUtilization metric.Float64Gauge
}

// InitOTelMetrics registers all VMAFX OTel metric instruments against mp.
// Errors during instrument creation degrade to no-ops rather than crashing.
func InitOTelMetrics(mp metric.MeterProvider, component string) *OTelMetrics {
	m := mp.Meter("vmafx/" + component)

	jobsQueued, _ := m.Int64UpDownCounter("vmafx.jobs.queued",
		metric.WithDescription("Current number of jobs waiting in the controller queue."),
		metric.WithUnit("{job}"),
	)
	jobsInFlight, _ := m.Int64UpDownCounter("vmafx.jobs.in_flight",
		metric.WithDescription("Number of jobs currently assigned to worker nodes."),
		metric.WithUnit("{job}"),
	)
	framesPerSec, _ := m.Float64Histogram("vmafx.frames_per_second",
		metric.WithDescription("Frame-extraction throughput in frames/second."),
		metric.WithUnit("fps"),
		metric.WithExplicitBucketBoundaries(1, 5, 10, 25, 50, 100, 250, 500),
	)
	scoreLatency, _ := m.Float64Histogram("vmafx.score_latency_ms",
		metric.WithDescription("End-to-end VMAF scoring latency in milliseconds."),
		metric.WithUnit("ms"),
		// Buckets tuned for p50/p99 discrimination on typical clip lengths.
		metric.WithExplicitBucketBoundaries(10, 50, 100, 250, 500, 1000, 2500, 5000, 10000),
	)
	gpuUtil, _ := m.Float64Gauge("vmafx.gpu_utilization",
		metric.WithDescription("GPU compute utilization percentage (0–100) reported by the node."),
		metric.WithUnit("%"),
	)

	return &OTelMetrics{
		JobsQueued:      jobsQueued,
		JobsInFlight:    jobsInFlight,
		FramesPerSecond: framesPerSec,
		ScoreLatency:    scoreLatency,
		GPUUtilization:  gpuUtil,
	}
}

// ---------------------------------------------------------------------------
// Span helpers
// ---------------------------------------------------------------------------

// StartSpan is a convenience wrapper around otel.Tracer that returns a span
// pre-populated with the given attributes.  Falls back to the global
// TracerProvider so callers that haven't called InitOTel still get no-ops.
func StartSpan(ctx context.Context, spanName string, attrs ...attribute.KeyValue) (context.Context, trace.Span) {
	return otel.Tracer("vmafx").Start(ctx, spanName, trace.WithAttributes(attrs...))
}

// EndSpan records the error (if any) and ends the span.
// Callers should defer EndSpan(span, &err) immediately after StartSpan.
func EndSpan(span trace.Span, errp *error) {
	if errp != nil && *errp != nil {
		span.RecordError(*errp)
		span.SetStatus(codes.Error, (*errp).Error())
	} else {
		span.SetStatus(codes.Ok, "")
	}
	span.End()
}

// ObserveScoreLatency records elapsed time (in ms) against the ScoreLatency
// histogram with the given attribute set.
//
// ctx is the request context — passing the caller's context rather than
// context.Background() preserves baggage and allows exemplar linking when
// the OTel SDK attaches trace IDs to histogram data points.
func ObserveScoreLatency(ctx context.Context, om *OTelMetrics, start time.Time, attrs ...attribute.KeyValue) {
	if om == nil {
		return
	}
	ms := float64(time.Since(start).Milliseconds())
	om.ScoreLatency.Record(ctx, ms, metric.WithAttributes(attrs...))
}
