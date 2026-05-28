// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxnode_controller.go — VmafxNode reconciler (Stage 1 stub).
//
// Stage 1 behaviour:
//   - Logs "Reconciling VmafxNode <namespace>/<name>".
//   - Resolves the node's /healthz endpoint from the controller's service labels
//     (label selector: app.kubernetes.io/name=vmafx-controller).
//   - Issues an HTTP GET /healthz with a 5-second timeout.
//   - Updates Healthy + LastHeartbeat in the status subresource.
//   - Requeues every 30 seconds regardless of health outcome.
//
// The healthz URL is constructed as http://<controller-service-ip>:<port>/healthz.
// The controller service is resolved via Kubernetes service labels.  In Stage 1
// the lookup is a best-effort DNS resolution; mTLS and cert rotation are Stage 2.
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.
// ADR-0709: VMAFX Phase 4b distributed platform (parent).

package controller

import (
	"context"
	"fmt"
	"net/http"
	"time"

	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/log"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
)

const (
	// nodeHealthzRequeueInterval is the period between health polls for each VmafxNode.
	nodeHealthzRequeueInterval = 30 * time.Second
	// nodeHealthzTimeout is the HTTP timeout for the /healthz probe.
	nodeHealthzTimeout = 5 * time.Second
	// controllerHealthzPort is the default HTTP port of the vmafx-controller service.
	controllerHealthzPort = 8080
)

// VmafxNodeReconciler reconciles VmafxNode objects.
type VmafxNodeReconciler struct {
	client.Client
	Scheme *runtime.Scheme
	// HTTPClient is injectable for testing; defaults to a short-timeout client.
	HTTPClient *http.Client
}

// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxnodes,verbs=get;list;watch;create;update;patch;delete
// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxnodes/status,verbs=get;update;patch
// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxnodes/finalizers,verbs=update

// Reconcile is the reconciliation loop for VmafxNode.
func (r *VmafxNodeReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
	logger := log.FromContext(ctx)
	logger.Info("Reconciling VmafxNode", "namespace", req.Namespace, "name", req.Name)

	// Fetch the VmafxNode instance.
	var node vmafxv1.VmafxNode
	if err := r.Get(ctx, req.NamespacedName, &node); err != nil {
		return ctrl.Result{}, client.IgnoreNotFound(err)
	}

	// Stage 1: poll /healthz on the controller service to confirm the node is reachable.
	// The controller's HTTP service is co-located in the same namespace by convention.
	// In Stage 2 the URL will be derived from Service discovery; here we use a label-based
	// DNS name: vmafx-controller.<namespace>.svc.cluster.local.
	healthy := r.probeHealthz(ctx, req.Namespace)

	now := metav1.NewTime(time.Now())
	node.Status.Healthy = healthy
	node.Status.LastHeartbeat = &now

	if err := r.Status().Update(ctx, &node); err != nil {
		logger.Error(err, "Failed to update VmafxNode status",
			"namespace", req.Namespace, "name", req.Name)
		return ctrl.Result{}, err
	}

	logger.Info("VmafxNode health polled",
		"namespace", req.Namespace, "name", req.Name,
		"healthy", healthy)

	// Requeue every 30 seconds to maintain a live heartbeat view.
	return ctrl.Result{RequeueAfter: nodeHealthzRequeueInterval}, nil
}

// probeHealthz issues an HTTP GET to the controller's /healthz endpoint.
// Returns true when the probe succeeds (HTTP 200); false on any error or
// non-200 status.
func (r *VmafxNodeReconciler) probeHealthz(ctx context.Context, namespace string) bool {
	hc := r.HTTPClient
	if hc == nil {
		hc = &http.Client{Timeout: nodeHealthzTimeout}
	}

	url := fmt.Sprintf("http://vmafx-controller.%s.svc.cluster.local:%d/healthz",
		namespace, controllerHealthzPort)

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return false
	}

	resp, err := hc.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close() //nolint:errcheck // body is empty on /healthz

	return resp.StatusCode == http.StatusOK
}

// SetupWithManager registers the VmafxNodeReconciler with the controller manager.
func (r *VmafxNodeReconciler) SetupWithManager(mgr ctrl.Manager) error {
	return ctrl.NewControllerManagedBy(mgr).
		For(&vmafxv1.VmafxNode{}).
		Complete(r)
}
