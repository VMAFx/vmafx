// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

package main

import (
	"context"
	"strings"
	"testing"
	"time"

	"google.golang.org/grpc/metadata"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

// TestGRPCTargetsComeFromEnv pins the "targets are never tool arguments" rule:
// a tool-supplied host would turn the MCP server into an SSRF pivot, so the
// addresses are env-only with a documented default.
func TestGRPCTargetsComeFromEnv(t *testing.T) {
	t.Setenv("VMAFX_CONTROLLER_ADDR", "")
	t.Setenv("VMAFX_SERVER_ADDR", "")
	if got := controllerAddr(); got != defaultControllerAddr {
		t.Errorf("controllerAddr default = %q, want %q", got, defaultControllerAddr)
	}
	if got := serverAddr(); got != defaultServerAddr {
		t.Errorf("serverAddr default = %q, want %q", got, defaultServerAddr)
	}
	t.Setenv("VMAFX_CONTROLLER_ADDR", "  ctrl.internal:7000  ")
	if got := controllerAddr(); got != "ctrl.internal:7000" {
		t.Errorf("controllerAddr = %q, want the trimmed override", got)
	}
	t.Setenv("VMAFX_SERVER_ADDR", "srv.internal:7001")
	if got := serverAddr(); got != "srv.internal:7001" {
		t.Errorf("serverAddr = %q, want the override", got)
	}
}

// TestGRPCTimeoutFallsBackClosed pins that a malformed or non-positive timeout
// keeps the default rather than disabling the deadline.
func TestGRPCTimeoutFallsBackClosed(t *testing.T) {
	for _, raw := range []string{"", "not-a-number", "0", "-5"} {
		t.Setenv("VMAFX_GRPC_TIMEOUT", raw)
		if got := grpcTimeout(); got != defaultGRPCTimeout {
			t.Errorf("grpcTimeout(%q) = %v, want %v", raw, got, defaultGRPCTimeout)
		}
	}
	t.Setenv("VMAFX_GRPC_TIMEOUT", "5")
	if got := grpcTimeout(); got != 5*time.Second {
		t.Errorf("grpcTimeout(\"5\") = %v, want 5s", got)
	}
}

// TestWithControllerAuthAttachesBearer pins the metadata contract the
// controller's auth interceptor expects.
func TestWithControllerAuthAttachesBearer(t *testing.T) {
	t.Setenv("VMAFX_CONTROLLER_TOKEN", "")
	ctx := withControllerAuth(context.Background())
	if _, ok := metadata.FromOutgoingContext(ctx); ok {
		t.Error("no token configured but outgoing metadata was set")
	}
	t.Setenv("VMAFX_CONTROLLER_TOKEN", "  s3cret  ")
	md, ok := metadata.FromOutgoingContext(withControllerAuth(context.Background()))
	if !ok {
		t.Fatal("expected outgoing metadata")
	}
	if got := md.Get("authorization"); len(got) != 1 || got[0] != "Bearer s3cret" {
		t.Errorf("authorization = %v, want [\"Bearer s3cret\"]", got)
	}
}

// TestValidateRemotePathRejectsUnsafeValues pins the remote-path shape check.
// libvmaf.ValidatePath deliberately does not apply here (the file lives on the
// worker), so this is the only guard those arguments get.
func TestValidateRemotePathRejectsUnsafeValues(t *testing.T) {
	bad := []struct {
		in   string
		want string
	}{
		{"", "is required"},
		{"   ", "is required"},
		{"relative/path.yuv", "must be an absolute path"},
		{"/mnt/data/../../etc/passwd", "'..' traversal"},
		{"/mnt/data/ref\nINJECTED", "control character"},
		{"/mnt/data/ref\x00.yuv", "control character"},
	}
	for _, tc := range bad {
		if _, err := validateRemotePath("reference", tc.in); err == nil {
			t.Errorf("validateRemotePath(%q) accepted an unsafe value", tc.in)
		} else if !strings.Contains(err.Error(), tc.want) {
			t.Errorf("validateRemotePath(%q) = %q, want it to mention %q", tc.in, err.Error(), tc.want)
		}
	}
	if got, err := validateRemotePath("reference", "/mnt/data/ref.yuv"); err != nil || got != "/mnt/data/ref.yuv" {
		t.Errorf("validateRemotePath rejected a valid path: %v / %q", err, got)
	}
}

// TestScoringParamsFromArgs pins backend normalisation and validation.
func TestScoringParamsFromArgs(t *testing.T) {
	base := map[string]any{"reference": "/mnt/ref.yuv", "distorted": "/mnt/dis.yuv"}

	p, err := scoringParamsFromArgs(base)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if p.GetModel() != "" {
		t.Errorf("model = %q, want \"\" (server default)", p.GetModel())
	}
	if p.GetBackend() != "" {
		t.Errorf("backend = %q, want \"\"", p.GetBackend())
	}

	withAuto := map[string]any{"reference": "/mnt/ref.yuv", "distorted": "/mnt/dis.yuv", "backend": "auto"}
	p, err = scoringParamsFromArgs(withAuto)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if p.GetBackend() != "" {
		t.Errorf("backend 'auto' must be spelled \"\" on the wire, got %q", p.GetBackend())
	}

	withCUDA := map[string]any{"reference": "/mnt/ref.yuv", "distorted": "/mnt/dis.yuv", "backend": "cuda"}
	p, err = scoringParamsFromArgs(withCUDA)
	if err != nil || p.GetBackend() != "cuda" {
		t.Errorf("backend cuda: err=%v backend=%q", err, p.GetBackend())
	}

	bad := map[string]any{"reference": "/mnt/ref.yuv", "distorted": "/mnt/dis.yuv", "backend": "opencl"}
	if _, err := scoringParamsFromArgs(bad); err == nil {
		t.Error("expected an error for an unknown backend")
	}
}

// TestJobToMap pins the response shape the tools return.
func TestJobToMap(t *testing.T) {
	job := &controllerv1.Job{
		Id:             "abc-123",
		Status:         controllerv1.JobStatus_RUNNING,
		AssignedNode:   "node-7",
		CreatedAt:      100,
		UpdatedAt:      200,
		FinalScore:     0,
		PartialResults: []float64{95.5, 96.0},
		Scoring: &controllerv1.ScoringParams{
			Reference: "/mnt/ref.yuv",
			Distorted: "/mnt/dis.yuv",
			Model:     "vmaf_v0.6.1",
			Backend:   "cuda",
		},
	}
	out := jobToMap(job)
	if out["status"] != "RUNNING" {
		t.Errorf("status = %v, want RUNNING", out["status"])
	}
	if out["id"] != "abc-123" || out["assigned_node"] != "node-7" {
		t.Errorf("unexpected identity fields: %v", out)
	}
	if out["partial_result_count"] != 2 {
		t.Errorf("partial_result_count = %v, want 2", out["partial_result_count"])
	}
	scoring, ok := out["scoring"].(map[string]any)
	if !ok || scoring["backend"] != "cuda" {
		t.Errorf("scoring block missing or wrong: %v", out["scoring"])
	}

	// A job with no partial results must not carry the key at all.
	empty := jobToMap(&controllerv1.Job{Id: "x", Status: controllerv1.JobStatus_PENDING})
	if _, present := empty["partial_results"]; present {
		t.Error("partial_results must be omitted when empty")
	}
	if empty["status"] != "PENDING" {
		t.Errorf("status = %v, want PENDING", empty["status"])
	}
}

// TestJobStatusMapsAreInverses guards against a status name drifting out of
// sync with the proto enum.
func TestJobStatusMapsAreInverses(t *testing.T) {
	if len(jobStatusNames) != len(jobStatusValues) {
		t.Fatalf("map sizes differ: %d names vs %d values", len(jobStatusNames), len(jobStatusValues))
	}
	for st, name := range jobStatusNames {
		if jobStatusValues[name] != st {
			t.Errorf("jobStatusValues[%q] = %v, want %v", name, jobStatusValues[name], st)
		}
		if controllerv1.JobStatus_name[int32(st)] != name {
			t.Errorf("proto enum name for %v is %q, tool map says %q",
				st, controllerv1.JobStatus_name[int32(st)], name)
		}
	}
}

// TestGRPCHandlersValidateBeforeDialing pins that argument validation happens
// before any network call, so a bad request fails fast even with no controller
// running.
func TestGRPCHandlersValidateBeforeDialing(t *testing.T) {
	ctx := context.Background()
	cases := []struct {
		name    string
		handler func(context.Context, map[string]any) (any, error)
		args    map[string]any
		want    string
	}{
		{"submit_job_relative_ref", handleSubmitJob,
			map[string]any{"reference": "ref.yuv", "distorted": "/mnt/dis.yuv"}, "absolute path"},
		{"submit_job_bad_backend", handleSubmitJob,
			map[string]any{"reference": "/mnt/r.yuv", "distorted": "/mnt/d.yuv", "backend": "opencl"}, "invalid backend"},
		{"get_job_missing_id", handleGetJob, map[string]any{}, "job_id is required"},
		{"cancel_job_blank_id", handleCancelJob, map[string]any{"job_id": "   "}, "job_id is required"},
		{"list_jobs_bad_limit", handleListJobs, map[string]any{"limit": 0}, "limit must be between"},
		{"list_jobs_bad_filter_type", handleListJobs, map[string]any{"status_filter": "PENDING"}, "must be an array"},
		{"list_jobs_bad_status", handleListJobs, map[string]any{"status_filter": []any{"QUEUED"}}, "invalid status"},
		{"score_remote_relative", handleVmafScoreRemote,
			map[string]any{"reference": "/mnt/r.yuv", "distorted": "d.yuv"}, "absolute path"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := tc.handler(ctx, tc.args)
			if err == nil {
				t.Fatalf("expected a validation error mentioning %q", tc.want)
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("error %q does not mention %q", err.Error(), tc.want)
			}
		})
	}
}

// TestListJobsStatusFilterIsCaseInsensitive pins the filter parsing: a
// lowercase status must never be the reason the call fails (the call may still
// fail later, when the dial to a controller that is not running times out).
func TestListJobsStatusFilterIsCaseInsensitive(t *testing.T) {
	if jobStatusValues["COMPLETED"] != controllerv1.JobStatus_COMPLETED {
		t.Fatal("status map does not carry COMPLETED")
	}
	t.Setenv("VMAFX_GRPC_TIMEOUT", "1")
	_, err := handleListJobs(context.Background(), map[string]any{
		"status_filter": []any{"completed"},
		"limit":         1,
	})
	if err != nil && strings.Contains(err.Error(), "invalid status") {
		t.Fatalf("lowercase status rejected: %v", err)
	}
}
