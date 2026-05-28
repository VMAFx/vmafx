// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxjob_controller.go — VmafxJob reconciler (Stage 1 stub).
//
// Stage 1 behaviour:
//   - Logs "Reconciling VmafxJob <namespace>/<name>".
//   - If Phase is empty, sets it to Pending and persists via status subresource.
//   - If AssignedNode is non-empty and Phase is still Pending, advances to Running.
//   - Otherwise, returns without requeue (controller will dispatch in Stage 2).
//
// Full Pod creation and score propagation are implemented in Stage 2 PRs.
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.
// ADR-0709: VMAFX Phase 4b distributed platform (parent).

package controller

import (
	"context"

	"k8s.io/apimachinery/pkg/runtime"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/log"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
)

// VmafxJobReconciler reconciles VmafxJob objects.
type VmafxJobReconciler struct {
	client.Client
	Scheme *runtime.Scheme
}

// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxjobs,verbs=get;list;watch;create;update;patch;delete
// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxjobs/status,verbs=get;update;patch
// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxjobs/finalizers,verbs=update
// +kubebuilder:rbac:groups="",resources=pods,verbs=get;list;watch;create;update;patch;delete

// Reconcile is the reconciliation loop for VmafxJob.
func (r *VmafxJobReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
	logger := log.FromContext(ctx)
	logger.Info("Reconciling VmafxJob", "namespace", req.Namespace, "name", req.Name)

	// Fetch the VmafxJob instance.
	var job vmafxv1.VmafxJob
	if err := r.Get(ctx, req.NamespacedName, &job); err != nil {
		// Object deleted — nothing to do.
		return ctrl.Result{}, client.IgnoreNotFound(err)
	}

	// Stage 1: initialise Phase if not set.
	if job.Status.Phase == "" {
		logger.Info("Initialising VmafxJob phase to Pending",
			"namespace", req.Namespace, "name", req.Name)

		job.Status.Phase = vmafxv1.VmafxJobPhasePending

		if err := r.Status().Update(ctx, &job); err != nil {
			logger.Error(err, "Failed to update VmafxJob status to Pending")
			return ctrl.Result{}, err
		}
		return ctrl.Result{}, nil
	}

	// Stage 1: if a node has been assigned externally (e.g. by the controller),
	// advance Pending -> Running so the status reflects active execution.
	if job.Status.Phase == vmafxv1.VmafxJobPhasePending && job.Status.AssignedNode != "" {
		logger.Info("VmafxJob assigned to node — advancing to Running",
			"namespace", req.Namespace, "name", req.Name,
			"assignedNode", job.Status.AssignedNode)

		job.Status.Phase = vmafxv1.VmafxJobPhaseRunning

		if err := r.Status().Update(ctx, &job); err != nil {
			logger.Error(err, "Failed to update VmafxJob status to Running")
			return ctrl.Result{}, err
		}
		return ctrl.Result{}, nil
	}

	// Stage 1: terminal states and Running are left for Stage 2 Pod reconciliation.
	logger.Info("VmafxJob is in phase; no action in Stage 1 stub",
		"namespace", req.Namespace, "name", req.Name,
		"phase", job.Status.Phase)

	return ctrl.Result{}, nil
}

// SetupWithManager registers the VmafxJobReconciler with the controller manager.
func (r *VmafxJobReconciler) SetupWithManager(mgr ctrl.Manager) error {
	return ctrl.NewControllerManagedBy(mgr).
		For(&vmafxv1.VmafxJob{}).
		Complete(r)
}
