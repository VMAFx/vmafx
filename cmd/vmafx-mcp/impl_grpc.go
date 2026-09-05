// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

// impl_grpc.go implements the Priority-3 gRPC bridge tools (epic #1240 item c):
// the MCP surface over the Phase-4b control plane.
//
//	submit_job        -> VmafxController.SubmitJob
//	get_job           -> VmafxController.GetJob
//	cancel_job        -> VmafxController.CancelJob
//	list_jobs         -> VmafxController.StreamJobs (snapshot, drained to a slice)
//	vmaf_score_remote -> VmafxScoring.Score on vmafx-server (pkg/score.Client)
//
// This is the edge the Phase-4b architecture prescribes:
// docs/architecture/phase4b-distributed-platform.md draws `MCP -->|gRPC| CTRL`,
// i.e. the MCP server is a thin client of the controller's Client API and does
// NOT talk to worker nodes or the job store directly.
//
// Connection targets and credentials come from the environment, not from tool
// arguments — a tool argument naming an arbitrary host would turn the MCP
// server into an SSRF pivot:
//
//	VMAFX_CONTROLLER_ADDR   controller gRPC address   (default "localhost:9090")
//	VMAFX_SERVER_ADDR       vmafx-server gRPC address (default "localhost:9090")
//	VMAFX_CONTROLLER_TOKEN  optional bearer token attached to every controller RPC
//	VMAFX_GRPC_TIMEOUT      per-RPC deadline in seconds (default 30)
//
// Python parity: these five tools are Go-only by decision (ADR-1173). The
// Python server is the ADR-0704 Stage-1 implementation whose whole point is to
// avoid a heavier wheel chain; adding grpcio + generated Python stubs there
// would re-import the dependency Phase 4 is removing. The parity contract is
// "Go is a superset of Python" (server_test.go::TestToolListMatchesPython), so
// Go-only tools are structurally allowed; the reverse divergence already exists
// (VLM `describe_worst_frames` output is Python-only).

package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/score"
)

const (
	// defaultControllerAddr / defaultServerAddr match the golusoris gRPC listen
	// default (":9090") used by cmd/vmafx-controller and cmd/vmafx-server.
	defaultControllerAddr = "localhost:9090"
	defaultServerAddr     = "localhost:9090"

	// defaultGRPCTimeout bounds every bridge RPC. Job submission and lookup are
	// control-plane calls that must never hang an MCP client.
	defaultGRPCTimeout = 30 * time.Second

	// listJobsHardLimit caps how many jobs a single list_jobs snapshot returns,
	// so a controller with a large backlog cannot blow up the MCP response.
	listJobsHardLimit = 500
)

// controllerAddr returns the controller gRPC target.
func controllerAddr() string {
	if v := strings.TrimSpace(os.Getenv("VMAFX_CONTROLLER_ADDR")); v != "" {
		return v
	}
	return defaultControllerAddr
}

// serverAddr returns the vmafx-server gRPC target for vmaf_score_remote.
func serverAddr() string {
	if v := strings.TrimSpace(os.Getenv("VMAFX_SERVER_ADDR")); v != "" {
		return v
	}
	return defaultServerAddr
}

// grpcTimeout returns the per-RPC deadline. A malformed or non-positive
// VMAFX_GRPC_TIMEOUT falls back to the default rather than disabling the
// deadline — failing open on a timeout is how MCP clients end up wedged.
func grpcTimeout() time.Duration {
	raw := strings.TrimSpace(os.Getenv("VMAFX_GRPC_TIMEOUT"))
	if raw == "" {
		return defaultGRPCTimeout
	}
	secs, err := strconv.Atoi(raw)
	if err != nil || secs <= 0 {
		return defaultGRPCTimeout
	}
	return time.Duration(secs) * time.Second
}

// withControllerAuth attaches the optional bearer token to the outgoing
// context. The controller's auth interceptor expects "authorization: Bearer T"
// (cmd/vmafx-controller/auth/grpc_interceptor.go); when no token is configured
// the context is returned unchanged so an auth-disabled controller still works.
func withControllerAuth(ctx context.Context) context.Context {
	tok := strings.TrimSpace(os.Getenv("VMAFX_CONTROLLER_TOKEN"))
	if tok == "" {
		return ctx
	}
	return metadata.AppendToOutgoingContext(ctx, "authorization", "Bearer "+tok)
}

// dialController opens a controller connection. The caller must close it.
//
// Transport is insecure by default, matching pkg/score.Dial — the control plane
// is expected to run inside the cluster mesh. A TLS story for both is the
// follow-up named in pkg/score/grpc_client.go.
func dialController() (*grpc.ClientConn, controllerv1.VmafxControllerClient, error) {
	addr := controllerAddr()
	conn, err := grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, nil, fmt.Errorf("dial controller %s: %w", addr, err)
	}
	return conn, controllerv1.NewVmafxControllerClient(conn), nil
}

// closeConn reports a close failure on stderr without failing the tool call —
// the RPC result is already computed by the time the connection is torn down.
func closeConn(conn *grpc.ClientConn) {
	if err := conn.Close(); err != nil {
		fmt.Fprintf(os.Stderr, "vmafx-mcp: closing controller connection: %v\n", err)
	}
}

// jobStatusNames maps the proto enum onto the wire strings the tools return.
var jobStatusNames = map[controllerv1.JobStatus]string{
	controllerv1.JobStatus_PENDING:   "PENDING",
	controllerv1.JobStatus_RUNNING:   "RUNNING",
	controllerv1.JobStatus_COMPLETED: "COMPLETED",
	controllerv1.JobStatus_FAILED:    "FAILED",
	controllerv1.JobStatus_CANCELLED: "CANCELLED",
}

// jobStatusValues is the inverse map used to validate list_jobs filters.
var jobStatusValues = map[string]controllerv1.JobStatus{
	"PENDING":   controllerv1.JobStatus_PENDING,
	"RUNNING":   controllerv1.JobStatus_RUNNING,
	"COMPLETED": controllerv1.JobStatus_COMPLETED,
	"FAILED":    controllerv1.JobStatus_FAILED,
	"CANCELLED": controllerv1.JobStatus_CANCELLED,
}

// jobToMap renders a controller Job as the tool response shape. Field names
// match the proto field names so a caller can read the proto as documentation.
func jobToMap(j *controllerv1.Job) map[string]any {
	out := map[string]any{
		"id":            j.GetId(),
		"status":        jobStatusNames[j.GetStatus()],
		"assigned_node": j.GetAssignedNode(),
		"error":         j.GetError(),
		"created_at":    j.GetCreatedAt(),
		"updated_at":    j.GetUpdatedAt(),
		"final_score":   j.GetFinalScore(),
	}
	if s := j.GetScoring(); s != nil {
		out["scoring"] = map[string]any{
			"reference": s.GetReference(),
			"distorted": s.GetDistorted(),
			"model":     s.GetModel(),
			"backend":   s.GetBackend(),
		}
	}
	if p := j.GetPartialResults(); len(p) > 0 {
		out["partial_results"] = p
		out["partial_result_count"] = len(p)
	}
	return out
}

// validateRemotePath checks a path that will be resolved on a *remote* node.
//
// libvmaf.ValidatePath is deliberately NOT used here: the controller resolves
// these against the worker's rclone mount, so the file does not have to exist
// on the MCP host and a local-allowlist check would reject every legitimate
// value. What we still enforce is that the value is an absolute, clean path
// with no NUL / newline injection and no ".." traversal component — the same
// shape the controller's own job store expects.
func validateRemotePath(field, p string) (string, error) {
	if strings.TrimSpace(p) == "" {
		return "", fmt.Errorf("%s is required", field)
	}
	if strings.ContainsAny(p, "\x00\n\r") {
		return "", fmt.Errorf("%s contains a control character", field)
	}
	if !strings.HasPrefix(p, "/") {
		return "", fmt.Errorf("%s must be an absolute path on the worker node (got %q)", field, p)
	}
	for _, seg := range strings.Split(p, "/") {
		if seg == ".." {
			return "", fmt.Errorf("%s must not contain a '..' traversal component", field)
		}
	}
	return p, nil
}

// scoringParamsFromArgs builds the ScoringParams shared by submit_job.
func scoringParamsFromArgs(args map[string]any) (*controllerv1.ScoringParams, error) {
	ref, err := validateRemotePath("reference", strArg(args, "reference", ""))
	if err != nil {
		return nil, err
	}
	dis, err := validateRemotePath("distorted", strArg(args, "distorted", ""))
	if err != nil {
		return nil, err
	}
	backend := strArg(args, "backend", "")
	// The controller matches this against a node capability string; "auto" is
	// spelled as the empty string on the wire (see controller.proto).
	if backend == "auto" {
		backend = ""
	}
	if backend != "" && !validBackends[backend] {
		return nil, fmt.Errorf(
			"invalid backend %q: must be one of auto|cpu|cuda|sycl|hip|metal", backend)
	}
	return &controllerv1.ScoringParams{
		Reference: ref,
		Distorted: dis,
		// Empty means "server default" on the wire (controller.proto), so an
		// unset model is forwarded as-is rather than pinned here.
		Model:   strArg(args, "model", ""),
		Backend: backend,
	}, nil
}

// ---------------------------------------------------------------------------
// Tool: submit_job
// ---------------------------------------------------------------------------

func handleSubmitJob(ctx context.Context, args map[string]any) (any, error) {
	params, err := scoringParamsFromArgs(args)
	if err != nil {
		return nil, err
	}
	conn, client, err := dialController()
	if err != nil {
		return nil, err
	}
	defer closeConn(conn)

	rpcCtx, cancel := context.WithTimeout(withControllerAuth(ctx), grpcTimeout())
	defer cancel()

	resp, err := client.SubmitJob(rpcCtx, &controllerv1.SubmitJobRequest{Scoring: params})
	if err != nil {
		return nil, fmt.Errorf("SubmitJob on %s: %w", controllerAddr(), err)
	}
	return map[string]any{
		"job_id":     resp.GetJobId(),
		"status":     "PENDING",
		"controller": controllerAddr(),
		"scoring": map[string]any{
			"reference": params.GetReference(),
			"distorted": params.GetDistorted(),
			"model":     params.GetModel(),
			"backend":   params.GetBackend(),
		},
	}, nil
}

// ---------------------------------------------------------------------------
// Tool: get_job
// ---------------------------------------------------------------------------

func handleGetJob(ctx context.Context, args map[string]any) (any, error) {
	jobID := strings.TrimSpace(strArg(args, "job_id", ""))
	if jobID == "" {
		return nil, fmt.Errorf("job_id is required")
	}
	conn, client, err := dialController()
	if err != nil {
		return nil, err
	}
	defer closeConn(conn)

	rpcCtx, cancel := context.WithTimeout(withControllerAuth(ctx), grpcTimeout())
	defer cancel()

	job, err := client.GetJob(rpcCtx, &controllerv1.GetJobRequest{JobId: jobID})
	if err != nil {
		return nil, fmt.Errorf("GetJob %s on %s: %w", jobID, controllerAddr(), err)
	}
	out := jobToMap(job)
	out["controller"] = controllerAddr()
	return out, nil
}

// ---------------------------------------------------------------------------
// Tool: cancel_job
// ---------------------------------------------------------------------------

func handleCancelJob(ctx context.Context, args map[string]any) (any, error) {
	jobID := strings.TrimSpace(strArg(args, "job_id", ""))
	if jobID == "" {
		return nil, fmt.Errorf("job_id is required")
	}
	conn, client, err := dialController()
	if err != nil {
		return nil, err
	}
	defer closeConn(conn)

	rpcCtx, cancel := context.WithTimeout(withControllerAuth(ctx), grpcTimeout())
	defer cancel()

	resp, err := client.CancelJob(rpcCtx, &controllerv1.CancelJobRequest{JobId: jobID})
	if err != nil {
		return nil, fmt.Errorf("CancelJob %s on %s: %w", jobID, controllerAddr(), err)
	}
	return map[string]any{
		"job_id":     jobID,
		"ok":         resp.GetOk(),
		"message":    resp.GetMessage(),
		"controller": controllerAddr(),
	}, nil
}

// ---------------------------------------------------------------------------
// Tool: list_jobs
// ---------------------------------------------------------------------------

// handleListJobs drains the StreamJobs snapshot into a slice.
//
// StreamJobs is a server-streaming RPC that (per ADR-0962) sends the current
// SQLite snapshot and then closes; it is not an open-ended subscription, so
// draining to io.EOF terminates. `limit` caps the number of jobs returned and
// the stream is abandoned (context cancelled) once the cap is hit.
func handleListJobs(ctx context.Context, args map[string]any) (any, error) {
	limit := intArg(args, "limit", 100)
	if limit < 1 || limit > listJobsHardLimit {
		return nil, fmt.Errorf("limit must be between 1 and %d", listJobsHardLimit)
	}
	var filter []controllerv1.JobStatus
	if raw, ok := args["status_filter"]; ok && raw != nil {
		list, isList := raw.([]any)
		if !isList {
			return nil, fmt.Errorf("status_filter must be an array of status strings")
		}
		for _, item := range list {
			name, isStr := item.(string)
			if !isStr {
				return nil, fmt.Errorf("status_filter entries must be strings")
			}
			st, known := jobStatusValues[strings.ToUpper(strings.TrimSpace(name))]
			if !known {
				return nil, fmt.Errorf(
					"invalid status %q: must be one of PENDING|RUNNING|COMPLETED|FAILED|CANCELLED",
					name)
			}
			filter = append(filter, st)
		}
	}

	conn, client, err := dialController()
	if err != nil {
		return nil, err
	}
	defer closeConn(conn)

	rpcCtx, cancel := context.WithTimeout(withControllerAuth(ctx), grpcTimeout())
	defer cancel()

	stream, err := client.StreamJobs(rpcCtx, &controllerv1.StreamJobsRequest{StatusFilter: filter})
	if err != nil {
		return nil, fmt.Errorf("StreamJobs on %s: %w", controllerAddr(), err)
	}
	jobs := make([]map[string]any, 0, limit)
	truncated := false
	for {
		job, recvErr := stream.Recv()
		if errors.Is(recvErr, io.EOF) {
			break
		}
		if recvErr != nil {
			return nil, fmt.Errorf("StreamJobs on %s: %w", controllerAddr(), recvErr)
		}
		if len(jobs) >= limit {
			truncated = true
			break
		}
		jobs = append(jobs, jobToMap(job))
	}
	return map[string]any{
		"jobs":       jobs,
		"count":      len(jobs),
		"truncated":  truncated,
		"limit":      limit,
		"controller": controllerAddr(),
	}, nil
}

// ---------------------------------------------------------------------------
// Tool: vmaf_score_remote
// ---------------------------------------------------------------------------

// handleVmafScoreRemote calls the unary VmafxScoring.Score RPC on vmafx-server.
//
// Unlike vmaf_score this never touches the local vmaf binary and never reads a
// local file: the paths are resolved by the remote server. It is the "score
// this pair over there" escape hatch for callers whose data lives on the
// cluster's shared mount rather than on the MCP host.
func handleVmafScoreRemote(ctx context.Context, args map[string]any) (any, error) {
	ref, err := validateRemotePath("reference", strArg(args, "reference", ""))
	if err != nil {
		return nil, err
	}
	dis, err := validateRemotePath("distorted", strArg(args, "distorted", ""))
	if err != nil {
		return nil, err
	}
	// Empty means "server default" on the wire (proto/vmafx.proto).
	model := strArg(args, "model", "")

	addr := serverAddr()
	client, err := score.Dial(addr)
	if err != nil {
		return nil, fmt.Errorf("dial vmafx-server %s: %w", addr, err)
	}
	defer func() {
		if closeErr := client.Close(); closeErr != nil {
			fmt.Fprintf(os.Stderr, "vmafx-mcp: closing vmafx-server connection: %v\n", closeErr)
		}
	}()

	rpcCtx, cancel := context.WithTimeout(ctx, grpcTimeout())
	defer cancel()

	value, features, err := client.Score(rpcCtx, ref, dis, model)
	if err != nil {
		return nil, fmt.Errorf("Score on %s: %w", addr, err)
	}
	return map[string]any{
		"score":     value,
		"features":  features,
		"model":     model,
		"reference": ref,
		"distorted": dis,
		"server":    addr,
	}, nil
}
