// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxnode_lastheartbeat_test.go —
// Regression tests for the VmafxNode LastHeartbeat ownership invariant.
//
// Background (ADR-1069):
//   status.LastHeartbeat is owned exclusively by the node agent via the
//   controller's Heartbeat RPC.  VmafxNodeReconciler.Reconcile must not
//   overwrite it.  A prior bug reset the field to "now" on every 30 s reconcile
//   cycle, which defeated stale detection — a dead agent's stale timestamp was
//   immediately clobbered, so nodes with dead agents would never stay unhealthy.
//
// These tests use the controller-runtime fake client and run without
// KUBEBUILDER_ASSETS, making them available in every CI lane.  The envtest
// suite in vmafxnode_controller_test.go provides integration coverage; these
// tests pin the invariant at the unit level.
//
// ADR-0786: vmafx-operator Stage 2.
// ADR-1069: VmafxNode LastHeartbeat ownership — node agent only.

package controller

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/apimachinery/pkg/types"
	clientgoscheme "k8s.io/client-go/kubernetes/scheme"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client/fake"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
)

// fakeScheme builds the runtime.Scheme with vmafx types registered,
// mirroring what suite_test.go does for envtest.
func fakeScheme(t *testing.T) *runtime.Scheme {
	t.Helper()
	s := runtime.NewScheme()
	if err := clientgoscheme.AddToScheme(s); err != nil {
		t.Fatalf("AddToScheme(clientgo): %v", err)
	}
	if err := vmafxv1.AddToScheme(s); err != nil {
		t.Fatalf("AddToScheme(vmafx): %v", err)
	}
	return s
}

// TestLastHeartbeatNotOverwrittenByReconcile_FreshNode asserts that a fresh
// VmafxNode with no pre-set LastHeartbeat ends with LastHeartbeat == nil after
// a healthy-probe reconcile.  If the operator wrote LastHeartbeat (the bug),
// it would be non-nil.
func TestLastHeartbeatNotOverwrittenByReconcile_FreshNode(t *testing.T) {
	t.Parallel()

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	defer srv.Close()

	sc := fakeScheme(t)
	node := &vmafxv1.VmafxNode{
		ObjectMeta: metav1.ObjectMeta{
			Name:      "fresh-node",
			Namespace: "default",
		},
		Spec: vmafxv1.VmafxNodeSpec{GPUVendor: "nvidia", Capacity: 1},
	}
	fakeClient := fake.NewClientBuilder().
		WithScheme(sc).
		WithObjects(node).
		WithStatusSubresource(node).
		Build()

	r := &VmafxNodeReconciler{
		Client:             fakeClient,
		Scheme:             sc,
		ControllerHTTPAddr: srv.URL,
	}

	_, err := r.Reconcile(context.Background(), ctrl.Request{
		NamespacedName: types.NamespacedName{Name: "fresh-node", Namespace: "default"},
	})
	if err != nil {
		t.Fatalf("Reconcile: %v", err)
	}

	var got vmafxv1.VmafxNode
	if err := fakeClient.Get(context.Background(),
		types.NamespacedName{Name: "fresh-node", Namespace: "default"}, &got); err != nil {
		t.Fatalf("Get: %v", err)
	}

	// ADR-1069: the operator must not write LastHeartbeat.
	if got.Status.LastHeartbeat != nil {
		t.Errorf("Reconcile wrote LastHeartbeat = %v; expected nil — "+
			"LastHeartbeat is owned by the node agent, not the operator (ADR-1069)",
			got.Status.LastHeartbeat)
	}
}

// TestLastHeartbeatNotOverwrittenByReconcile_PreexistingTimestamp asserts that
// a stale pre-existing LastHeartbeat is left UNCHANGED after reconcile.
// The bug would reset it to "now" on every reconcile cycle.
func TestLastHeartbeatNotOverwrittenByReconcile_PreexistingTimestamp(t *testing.T) {
	t.Parallel()

	// Fake 200 /healthz — the stale check still fires first.
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	defer srv.Close()

	sc := fakeScheme(t)
	// Truncate to seconds: metav1.Time serialises to RFC3339 (second precision),
	// so a nanosecond-precision value would differ from the deserialized form and
	// cause a false-positive Equal failure.
	staleTime := metav1.NewTime(time.Now().Add(-90 * time.Second).Truncate(time.Second))
	node := &vmafxv1.VmafxNode{
		ObjectMeta: metav1.ObjectMeta{
			Name:      "stale-node",
			Namespace: "default",
		},
		Spec: vmafxv1.VmafxNodeSpec{GPUVendor: "intel"},
	}
	fakeClient := fake.NewClientBuilder().
		WithScheme(sc).
		WithObjects(node).
		WithStatusSubresource(node).
		Build()

	// Pre-set the stale heartbeat.
	node.Status.Healthy = true
	node.Status.LastHeartbeat = &staleTime
	if err := fakeClient.Status().Update(context.Background(), node); err != nil {
		t.Fatalf("Status().Update pre-set: %v", err)
	}

	r := &VmafxNodeReconciler{
		Client:             fakeClient,
		Scheme:             sc,
		ControllerHTTPAddr: srv.URL,
	}
	_, err := r.Reconcile(context.Background(), ctrl.Request{
		NamespacedName: types.NamespacedName{Name: "stale-node", Namespace: "default"},
	})
	if err != nil {
		t.Fatalf("Reconcile: %v", err)
	}

	var got vmafxv1.VmafxNode
	if err := fakeClient.Get(context.Background(),
		types.NamespacedName{Name: "stale-node", Namespace: "default"}, &got); err != nil {
		t.Fatalf("Get: %v", err)
	}

	// Healthy must be false (stale heartbeat detected).
	if got.Status.Healthy {
		t.Error("node with stale heartbeat should be unhealthy after reconcile")
	}

	// ADR-1069: LastHeartbeat must NOT be updated by the operator.
	if got.Status.LastHeartbeat == nil {
		t.Fatal("LastHeartbeat was cleared by reconcile; expected the pre-set stale timestamp")
	}
	if !got.Status.LastHeartbeat.Equal(&staleTime) {
		t.Errorf("LastHeartbeat changed from %v to %v — "+
			"operator must not modify LastHeartbeat (ADR-1069)",
			staleTime, got.Status.LastHeartbeat)
	}
}

// TestLastHeartbeatNotOverwrittenByReconcile_UnhealthyProbe asserts that a
// failing /healthz probe does not cause the operator to write LastHeartbeat.
func TestLastHeartbeatNotOverwrittenByReconcile_UnhealthyProbe(t *testing.T) {
	t.Parallel()

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer srv.Close()

	sc := fakeScheme(t)
	node := &vmafxv1.VmafxNode{
		ObjectMeta: metav1.ObjectMeta{
			Name:      "unhealthy-node",
			Namespace: "default",
		},
		Spec: vmafxv1.VmafxNodeSpec{GPUVendor: "amd", Capacity: 2},
	}
	fakeClient := fake.NewClientBuilder().
		WithScheme(sc).
		WithObjects(node).
		WithStatusSubresource(node).
		Build()

	r := &VmafxNodeReconciler{
		Client:             fakeClient,
		Scheme:             sc,
		ControllerHTTPAddr: srv.URL,
	}
	_, err := r.Reconcile(context.Background(), ctrl.Request{
		NamespacedName: types.NamespacedName{Name: "unhealthy-node", Namespace: "default"},
	})
	if err != nil {
		t.Fatalf("Reconcile: %v", err)
	}

	var got vmafxv1.VmafxNode
	if err := fakeClient.Get(context.Background(),
		types.NamespacedName{Name: "unhealthy-node", Namespace: "default"}, &got); err != nil {
		t.Fatalf("Get: %v", err)
	}

	if got.Status.Healthy {
		t.Error("node with failing /healthz probe should be unhealthy after reconcile")
	}
	// ADR-1069: operator must not write LastHeartbeat even on probe failure.
	if got.Status.LastHeartbeat != nil {
		t.Errorf("Reconcile wrote LastHeartbeat on unhealthy probe = %v; expected nil (ADR-1069)",
			got.Status.LastHeartbeat)
	}
}
