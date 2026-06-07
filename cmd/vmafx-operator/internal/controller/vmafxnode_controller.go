// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxnode_controller.go — VmafxNode reconciler.
//
// Reconciler behaviour:
//   - Probes the vmafx-controller /healthz endpoint every nodeProbeInterval (30 s).
//   - Updates status.Healthy on each probe; does NOT overwrite status.LastHeartbeat.
//   - status.LastHeartbeat is written exclusively by the node agent via the
//     controller's Heartbeat RPC; overwriting it here would defeat stale detection.
//   - Marks Healthy = false (without touching LastHeartbeat) when:
//       (a) the HTTP probe fails, OR
//       (b) LastHeartbeat is older than nodeStaleThreshold (60 s).
//   - The stale-threshold check fires even when the probe would succeed,
//     covering the case where a node's own heartbeat call to the controller
//     has stopped (distinct from the operator-side liveness probe).
//
// The probe URL is http://vmafx-controller.<namespace>.svc.cluster.local:8080/healthz
// by default; overridden by VMAFX_CONTROLLER_HTTP_ADDR for testing.
//
// ADR-0786: vmafx-operator Stage 2 — reconciler loops + webhook + per-controller RBAC.
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs (parent).

package controller

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"os"
	"time"

	"k8s.io/apimachinery/pkg/runtime"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/log"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
)

const (
	// nodeProbeInterval is the period between health probes for each VmafxNode.
	nodeProbeInterval = 30 * time.Second
	// nodeStaleThreshold is the maximum age of LastHeartbeat before the node is
	// considered stale and Healthy is set to false.
	nodeStaleThreshold = 60 * time.Second
	// nodeProbeTimeout is the HTTP timeout for the /healthz probe.
	nodeProbeTimeout = 5 * time.Second
	// controllerHTTPPort is the default HTTP port of the vmafx-controller service.
	controllerHTTPPort = 8080
)

// VmafxNodeReconciler reconciles VmafxNode objects.
type VmafxNodeReconciler struct {
	client.Client
	Scheme *runtime.Scheme
	// HTTPClient is injectable for testing; defaults to a short-timeout client.
	HTTPClient *http.Client
	// ControllerHTTPAddr overrides VMAFX_CONTROLLER_HTTP_ADDR when set (tests).
	ControllerHTTPAddr string
}

// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxnodes,verbs=get;list;watch;update;patch
// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxnodes/status,verbs=get;update;patch
// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxnodes/finalizers,verbs=update

// Reconcile is the reconciliation loop for VmafxNode.
func (r *VmafxNodeReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
	logger := log.FromContext(ctx)
	logger.Info("Reconciling VmafxNode", "namespace", req.Namespace, "name", req.Name)

	var node vmafxv1.VmafxNode
	if err := r.Get(ctx, req.NamespacedName, &node); err != nil {
		return ctrl.Result{}, client.IgnoreNotFound(err)
	}

	now := time.Now()

	// --- Stale-heartbeat check -----------------------------------------------
	// A node is stale when its own LastHeartbeat (written by the node agent via
	// the controller's Heartbeat RPC) has not been updated for nodeStaleThreshold.
	// This is separate from the operator's /healthz probe of the controller service.
	//
	// IMPORTANT: Do NOT overwrite node.Status.LastHeartbeat here.  It is owned
	// exclusively by the node agent; resetting it to now() on every reconcile
	// would prevent the stale-threshold from ever firing after the first cycle
	// (the field would always appear fresh). (ADR-1069)
	stale := false
	if node.Status.LastHeartbeat != nil &&
		now.Sub(node.Status.LastHeartbeat.Time) > nodeStaleThreshold {
		stale = true
		logger.Info("VmafxNode heartbeat is stale — marking unhealthy",
			"namespace", req.Namespace, "name", req.Name,
			"lastHeartbeat", node.Status.LastHeartbeat.Time,
			"age", now.Sub(node.Status.LastHeartbeat.Time))
	}

	// --- Controller /healthz probe -------------------------------------------
	probeHealthy := !stale && r.probeControllerHealthz(ctx, req.Namespace)

	// --- Persist status -------------------------------------------------------
	// Only update Healthy; LastHeartbeat is set by the node agent, not the operator.
	prevHealthy := node.Status.Healthy
	node.Status.Healthy = probeHealthy

	if err := r.Status().Update(ctx, &node); err != nil {
		logger.Error(err, "Failed to update VmafxNode status")
		return ctrl.Result{}, err
	}

	if prevHealthy != probeHealthy {
		logger.Info("VmafxNode health changed",
			"namespace", req.Namespace, "name", req.Name,
			"healthy", probeHealthy, "stale", stale)
	}

	return ctrl.Result{RequeueAfter: nodeProbeInterval}, nil
}

// probeControllerHealthz issues an HTTP GET to the controller's /healthz endpoint.
// Returns true when the probe succeeds (HTTP 200); false on any error or non-200 status.
func (r *VmafxNodeReconciler) probeControllerHealthz(ctx context.Context, namespace string) bool {
	return r.probeHealthz(ctx, namespace)
}

// probeHealthz performs the HTTP /healthz probe for the named namespace.
// The body is fully drained before closing so Go's net/http keep-alive pool
// can reuse the underlying TCP connection — without draining, each call leaks
// one connection per polled node (ADR-0786 operator audit).
func (r *VmafxNodeReconciler) probeHealthz(ctx context.Context, namespace string) bool {
	hc := r.HTTPClient
	// HTTPClient is always initialised by SetupWithManager; the nil fallback is
	// only reached in tests that construct the reconciler directly without calling
	// SetupWithManager (r5-scheduler-timer, ADR-1017).
	if hc == nil {
		hc = &http.Client{Timeout: nodeProbeTimeout}
	}

	url := r.resolveControllerHTTPURL(namespace)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return false
	}

	resp, err := hc.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close() //nolint:errcheck // drain error not actionable
	// Drain fully before Close so the HTTP/1.1 keep-alive connection can be
	// returned to the pool. An undrained body causes a new TCP dial on the
	// next probe, leaking one FD per polled node every 30 s (ADR-0786 audit).
	_, _ = io.Copy(io.Discard, resp.Body)

	return resp.StatusCode == http.StatusOK
}

// resolveControllerHTTPURL returns the /healthz URL for the vmafx-controller service.
func (r *VmafxNodeReconciler) resolveControllerHTTPURL(namespace string) string {
	if r.ControllerHTTPAddr != "" {
		return r.ControllerHTTPAddr + "/healthz"
	}
	if v := os.Getenv("VMAFX_CONTROLLER_HTTP_ADDR"); v != "" {
		return v + "/healthz"
	}
	return fmt.Sprintf("http://vmafx-controller.%s.svc.cluster.local:%d/healthz",
		namespace, controllerHTTPPort)
}

// SetupWithManager registers the VmafxNodeReconciler with the controller manager.
// It also initialises the default HTTP client when the caller has not injected
// one (production path), so Reconcile always uses a shared client that benefits
// from keep-alive connection pooling instead of allocating a fresh one per call
// (r5-scheduler-timer, ADR-1017).
func (r *VmafxNodeReconciler) SetupWithManager(mgr ctrl.Manager) error {
	if r.HTTPClient == nil {
		r.HTTPClient = &http.Client{Timeout: nodeProbeTimeout}
	}
	return ctrl.NewControllerManagedBy(mgr).
		For(&vmafxv1.VmafxNode{}).
		Complete(r)
}
