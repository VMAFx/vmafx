// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxjob_applystatus_test.go —
// table-driven unit tests for VmafxJobReconciler.applyRemoteStatus and
// VmafxJobReconciler.resolveControllerAddr.
//
// These functions are pure (no k8s API calls) so they can be tested without
// envtest or a live controller.  The Reconcile loop itself is exercised by the
// Ginkgo suite in suite_test.go; this file covers the status-mapping logic only.
//
// ADR-0786: vmafx-operator Stage 2 — reconciler loops.

package controller

import (
	"os"
	"testing"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
)

// TestApplyRemoteStatus_AllTransitions verifies that applyRemoteStatus
// maps every remote JobStatus value onto the expected VmafxJob phase.
func TestApplyRemoteStatus_AllTransitions(t *testing.T) {
	t.Parallel()
	tests := []struct {
		name       string
		remote     controllerv1.JobStatus
		wantPhase  vmafxv1.VmafxJobPhase
		wantChange bool
	}{
		{
			name:       "PENDING → Pending",
			remote:     controllerv1.JobStatus_PENDING,
			wantPhase:  vmafxv1.VmafxJobPhasePending,
			wantChange: true,
		},
		{
			name:       "RUNNING → Running",
			remote:     controllerv1.JobStatus_RUNNING,
			wantPhase:  vmafxv1.VmafxJobPhaseRunning,
			wantChange: true,
		},
		{
			name:       "COMPLETED → Succeeded",
			remote:     controllerv1.JobStatus_COMPLETED,
			wantPhase:  vmafxv1.VmafxJobPhaseSucceeded,
			wantChange: true,
		},
		{
			name:       "FAILED → Failed",
			remote:     controllerv1.JobStatus_FAILED,
			wantPhase:  vmafxv1.VmafxJobPhaseFailed,
			wantChange: true,
		},
		{
			name:       "CANCELLED → Failed",
			remote:     controllerv1.JobStatus_CANCELLED,
			wantPhase:  vmafxv1.VmafxJobPhaseFailed,
			wantChange: true,
		},
	}

	r := &VmafxJobReconciler{}
	for _, tt := range tests {
		tt := tt
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			cr := &vmafxv1.VmafxJob{}
			remote := &controllerv1.Job{
				Status:     tt.remote,
				FinalScore: 76.6683,
				Error:      "test error",
			}
			changed := r.applyRemoteStatus(cr, remote)
			if changed != tt.wantChange {
				t.Errorf("changed = %v, want %v", changed, tt.wantChange)
			}
			if cr.Status.Phase != tt.wantPhase {
				t.Errorf("phase = %q, want %q", cr.Status.Phase, tt.wantPhase)
			}
		})
	}
}

// TestApplyRemoteStatus_Idempotent verifies that a second call with the same
// remote status does NOT set changed=true (avoids spurious k8s Status updates).
func TestApplyRemoteStatus_Idempotent(t *testing.T) {
	t.Parallel()
	r := &VmafxJobReconciler{}
	cr := &vmafxv1.VmafxJob{}
	remote := &controllerv1.Job{Status: controllerv1.JobStatus_PENDING}

	// First application.
	changed1 := r.applyRemoteStatus(cr, remote)
	if !changed1 {
		t.Fatal("first application: expected changed=true")
	}
	// Second application with identical status.
	changed2 := r.applyRemoteStatus(cr, remote)
	if changed2 {
		t.Errorf("second application: expected changed=false (idempotent), got true")
	}
}

// TestApplyRemoteStatus_CompletedSetsScore verifies that COMPLETED copies the
// FinalScore from the remote job.
func TestApplyRemoteStatus_CompletedSetsScore(t *testing.T) {
	t.Parallel()
	r := &VmafxJobReconciler{}
	cr := &vmafxv1.VmafxJob{}
	remote := &controllerv1.Job{
		Status:     controllerv1.JobStatus_COMPLETED,
		FinalScore: 92.1234,
	}
	r.applyRemoteStatus(cr, remote)
	if cr.Status.Score != 92.1234 {
		t.Errorf("score: got %v, want 92.1234", cr.Status.Score)
	}
	if cr.Status.FinishedAt == nil {
		t.Error("FinishedAt should be set on COMPLETED")
	}
}

// TestApplyRemoteStatus_FailedSetsMessage verifies that FAILED copies the
// error message from the remote job.
func TestApplyRemoteStatus_FailedSetsMessage(t *testing.T) {
	t.Parallel()
	r := &VmafxJobReconciler{}
	cr := &vmafxv1.VmafxJob{}
	remote := &controllerv1.Job{
		Status: controllerv1.JobStatus_FAILED,
		Error:  "libvmaf exit 1: bad input",
	}
	r.applyRemoteStatus(cr, remote)
	if cr.Status.Message != "libvmaf exit 1: bad input" {
		t.Errorf("message: got %q", cr.Status.Message)
	}
}

// TestApplyRemoteStatus_AssignedNodePropagated verifies that AssignedNode is
// copied from the remote job when it is non-empty.
func TestApplyRemoteStatus_AssignedNodePropagated(t *testing.T) {
	t.Parallel()
	r := &VmafxJobReconciler{}
	cr := &vmafxv1.VmafxJob{}
	remote := &controllerv1.Job{
		Status:       controllerv1.JobStatus_RUNNING,
		AssignedNode: "node-gpu-1",
	}
	changed := r.applyRemoteStatus(cr, remote)
	if !changed {
		t.Fatal("expected changed=true when AssignedNode is set")
	}
	if cr.Status.AssignedNode != "node-gpu-1" {
		t.Errorf("assignedNode: got %q, want %q", cr.Status.AssignedNode, "node-gpu-1")
	}
}

// TestResolveControllerAddr_FieldOverride tests the ControllerAddr struct field.
func TestResolveControllerAddr_FieldOverride(t *testing.T) {
	t.Parallel()
	r := &VmafxJobReconciler{ControllerAddr: "custom-controller:9090"}
	if got := r.resolveControllerAddr("default"); got != "custom-controller:9090" {
		t.Errorf("got %q, want %q", got, "custom-controller:9090")
	}
}

// TestResolveControllerAddr_EnvOverride tests the VMAFX_CONTROLLER_GRPC_ADDR
// environment variable path.
func TestResolveControllerAddr_EnvOverride(t *testing.T) {
	// Not parallel — uses os.Setenv.
	const key = "VMAFX_CONTROLLER_GRPC_ADDR"
	t.Setenv(key, "env-controller:9090")
	r := &VmafxJobReconciler{}
	if got := r.resolveControllerAddr("default"); got != "env-controller:9090" {
		t.Errorf("got %q, want %q", got, "env-controller:9090")
	}
	if err := os.Unsetenv(key); err != nil {
		t.Fatalf("Unsetenv: %v", err)
	}
}

// TestResolveControllerAddr_DefaultFormat tests the in-cluster DNS default.
func TestResolveControllerAddr_DefaultFormat(t *testing.T) {
	t.Parallel()
	// Ensure the env var is not set (it might be set from a prior test run on
	// the same binary if tests share state).
	if v := os.Getenv("VMAFX_CONTROLLER_GRPC_ADDR"); v != "" {
		t.Skipf("VMAFX_CONTROLLER_GRPC_ADDR is set to %q; skipping default format test", v)
	}
	r := &VmafxJobReconciler{}
	got := r.resolveControllerAddr("my-namespace")
	want := "vmafx-controller.my-namespace.svc.cluster.local:9090"
	if got != want {
		t.Errorf("got %q, want %q", got, want)
	}
}
