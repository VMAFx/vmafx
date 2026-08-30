// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxjob_controllerid_test.go —
// Tests for ControllerJobID round-trip correctness and the Reconcile branch
// that advances a job once ControllerJobID is set.
//
// Background (ADR-1069):
//   ControllerJobID was declared in VmafxJobStatus but missing from the CRD
//   YAML schema.  Kubernetes structural schema pruning silently drops unknown
//   status fields on every Status().Update() call, causing the reconciler to
//   loop forever in Pending (it saw ControllerJobID = "" even after the
//   external scheduler had set it).
//
//   The fix adds the field to both CRD files.  These tests:
//     (a) Verify the reconciler correctly reads ControllerJobID when present.
//     (b) Verify that a job with ControllerJobID set and a failing gRPC dial
//         still requeues (not errors out) — the expected steady-state when the
//         controller is temporarily unreachable.
//     (c) Verify the full Pending → no-grpc-dial path stays clean.
//
// Tests use the controller-runtime fake client and run without
// KUBEBUILDER_ASSETS.  The fake client does NOT enforce CRD structural
// schema pruning, so ControllerJobID round-trips correctly here even
// without the CRD fix.  The envtest suite is where CRD pruning would be
// caught; add an envtest test for the persisted field when #759 merges.
//
// ADR-0786: vmafx-operator Stage 2.
// ADR-1069: VmafxJob ControllerJobID CRD schema gap.

package controller

import (
	"context"
	"testing"

	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/types"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client/fake"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
)

// TestControllerJobID_SetThenRead verifies that a ControllerJobID written to
// status survives a Status().Update() → Get() round-trip using the fake
// client.  The fake client bypasses CRD structural schema pruning; this test
// documents the expected round-trip contract.  If the CRD fix from ADR-1069
// is not applied, the envtest version of this test would fail.
func TestControllerJobID_SetThenRead(t *testing.T) {
	t.Parallel()

	sc := fakeScheme(t)
	job := &vmafxv1.VmafxJob{
		ObjectMeta: metav1.ObjectMeta{
			Name:      "job-with-id",
			Namespace: "default",
		},
		Spec: vmafxv1.VmafxJobSpec{
			Reference: "file:///ref.yuv",
			Distorted: "file:///dis.yuv",
		},
	}
	fakeClient := fake.NewClientBuilder().
		WithScheme(sc).
		WithObjects(job).
		WithStatusSubresource(job).
		Build()

	const wantID = "ctrl-job-uuid-42"

	// Simulate the external scheduler setting ControllerJobID.
	job.Status.Phase = vmafxv1.VmafxJobPhasePending
	job.Status.ControllerJobID = wantID
	if err := fakeClient.Status().Update(context.Background(), job); err != nil {
		t.Fatalf("Status().Update: %v", err)
	}

	// Read it back — must not be pruned.
	var got vmafxv1.VmafxJob
	if err := fakeClient.Get(context.Background(),
		types.NamespacedName{Name: "job-with-id", Namespace: "default"}, &got); err != nil {
		t.Fatalf("Get: %v", err)
	}
	if got.Status.ControllerJobID != wantID {
		t.Errorf("ControllerJobID = %q after Status().Update() + Get(); want %q — "+
			"this documents the expected round-trip; CRD pruning would drop it in envtest",
			got.Status.ControllerJobID, wantID)
	}
}

// TestControllerJobID_ReconcileRequeuesWhenGRPCUnreachable verifies that when
// ControllerJobID is set but the gRPC dial fails (no controller running), the
// reconciler returns (RequeueAfter, nil) rather than propagating the error.
// The grpc dial failure is expected in environments without a live controller;
// the reconciler must absorb it and retry later.
func TestControllerJobID_ReconcileRequeuesWhenGRPCUnreachable(t *testing.T) {
	t.Parallel()

	sc := fakeScheme(t)
	job := &vmafxv1.VmafxJob{
		ObjectMeta: metav1.ObjectMeta{
			Name:      "job-grpc-fail",
			Namespace: "default",
		},
		Spec: vmafxv1.VmafxJobSpec{
			Reference: "file:///ref.yuv",
			Distorted: "file:///dis.yuv",
		},
	}
	fakeClient := fake.NewClientBuilder().
		WithScheme(sc).
		WithObjects(job).
		WithStatusSubresource(job).
		Build()

	// Pre-set Pending + ControllerJobID to reach the gRPC poll branch.
	job.Status.Phase = vmafxv1.VmafxJobPhasePending
	job.Status.ControllerJobID = "unreachable-ctrl-uuid"
	if err := fakeClient.Status().Update(context.Background(), job); err != nil {
		t.Fatalf("Status().Update: %v", err)
	}

	r := &VmafxJobReconciler{
		Client: fakeClient,
		Scheme: sc,
		// ControllerAddr points to a guaranteed-unreachable address.
		// Use the IANA-reserved TEST-NET-1 range (192.0.2.0/24) to ensure
		// the dial times out without touching any real network resource.
		ControllerAddr: "192.0.2.1:9090",
	}

	result, err := r.Reconcile(context.Background(), ctrl.Request{
		NamespacedName: types.NamespacedName{Name: "job-grpc-fail", Namespace: "default"},
	})
	// The reconciler must not surface the dial error — it should absorb it and requeue.
	if err != nil {
		t.Errorf("Reconcile returned error on gRPC failure: %v; expected nil (should requeue instead)", err)
	}
	if result.RequeueAfter == 0 {
		t.Errorf("Reconcile returned RequeueAfter=0 after gRPC failure; expected > 0")
	}
}

// TestControllerJobID_ReconcilePendingNoID verifies that a Pending job with
// no ControllerJobID does not attempt a gRPC dial and requeues cleanly.
func TestControllerJobID_ReconcilePendingNoID(t *testing.T) {
	t.Parallel()

	sc := fakeScheme(t)
	job := &vmafxv1.VmafxJob{
		ObjectMeta: metav1.ObjectMeta{
			Name:      "job-no-id",
			Namespace: "default",
		},
		Spec: vmafxv1.VmafxJobSpec{
			Reference: "file:///ref.yuv",
			Distorted: "file:///dis.yuv",
		},
	}
	fakeClient := fake.NewClientBuilder().
		WithScheme(sc).
		WithObjects(job).
		WithStatusSubresource(job).
		Build()

	// First reconcile: sets Phase to Pending.
	r := &VmafxJobReconciler{
		Client: fakeClient,
		Scheme: sc,
	}
	nn := types.NamespacedName{Name: "job-no-id", Namespace: "default"}
	result1, err := r.Reconcile(context.Background(), ctrl.Request{NamespacedName: nn})
	if err != nil {
		t.Fatalf("first Reconcile: %v", err)
	}
	if result1.RequeueAfter == 0 {
		t.Error("first reconcile: expected RequeueAfter > 0")
	}

	// Second reconcile: Pending with no ControllerJobID — must not dial gRPC.
	result2, err := r.Reconcile(context.Background(), ctrl.Request{NamespacedName: nn})
	if err != nil {
		t.Fatalf("second Reconcile: %v", err)
	}
	if result2.RequeueAfter == 0 {
		t.Error("second reconcile (no ControllerJobID): expected RequeueAfter > 0")
	}

	var got vmafxv1.VmafxJob
	if err := fakeClient.Get(context.Background(), nn, &got); err != nil {
		t.Fatalf("Get: %v", err)
	}
	if got.Status.ControllerJobID != "" {
		t.Errorf("ControllerJobID should remain empty; got %q", got.Status.ControllerJobID)
	}
	if got.Status.Phase != vmafxv1.VmafxJobPhasePending {
		t.Errorf("Phase should be Pending; got %q", got.Status.Phase)
	}
}
