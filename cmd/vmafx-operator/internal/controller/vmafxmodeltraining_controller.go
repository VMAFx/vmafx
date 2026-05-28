// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxmodeltraining_controller.go — VmafxModelTraining reconciler (Stage 1 stub).
//
// Stage 1 behaviour:
//   - Logs "Reconciling VmafxModelTraining <namespace>/<name>".
//   - If Phase is empty, sets it to Initializing.
//   - Requeues every 60 seconds; full online-SGD-EMA training loop is Stage 2.
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.
// ADR-0709: VMAFX Phase 4b distributed platform (parent).

package controller

import (
	"context"
	"time"

	"k8s.io/apimachinery/pkg/runtime"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/log"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
)

const (
	// trainingRequeueInterval is the period between stub reconcile runs.
	trainingRequeueInterval = 60 * time.Second
)

// VmafxModelTrainingReconciler reconciles VmafxModelTraining objects.
type VmafxModelTrainingReconciler struct {
	client.Client
	Scheme *runtime.Scheme
}

// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxmodeltrainings,verbs=get;list;watch;create;update;patch;delete
// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxmodeltrainings/status,verbs=get;update;patch
// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxmodeltrainings/finalizers,verbs=update

// Reconcile is the reconciliation loop for VmafxModelTraining.
func (r *VmafxModelTrainingReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
	logger := log.FromContext(ctx)
	logger.Info("Reconciling VmafxModelTraining",
		"namespace", req.Namespace, "name", req.Name)

	// Fetch the VmafxModelTraining instance.
	var mt vmafxv1.VmafxModelTraining
	if err := r.Get(ctx, req.NamespacedName, &mt); err != nil {
		return ctrl.Result{}, client.IgnoreNotFound(err)
	}

	// Stage 1: initialise Phase if not set.
	if mt.Status.Phase == "" {
		logger.Info("Initialising VmafxModelTraining phase to Initializing",
			"namespace", req.Namespace, "name", req.Name)

		mt.Status.Phase = vmafxv1.VmafxModelTrainingPhaseInitializing

		if err := r.Status().Update(ctx, &mt); err != nil {
			logger.Error(err, "Failed to initialise VmafxModelTraining phase")
			return ctrl.Result{}, err
		}
	}

	// Stage 1: log current state and requeue; full training logic is Stage 2.
	logger.Info("VmafxModelTraining Stage 1 stub — requeuing",
		"namespace", req.Namespace, "name", req.Name,
		"phase", mt.Status.Phase,
		"samples", mt.Status.CurrentSamples,
		"baseModel", mt.Spec.BaseModel,
		"algorithm", mt.Spec.Algorithm)

	return ctrl.Result{RequeueAfter: trainingRequeueInterval}, nil
}

// SetupWithManager registers the VmafxModelTrainingReconciler with the controller manager.
func (r *VmafxModelTrainingReconciler) SetupWithManager(mgr ctrl.Manager) error {
	return ctrl.NewControllerManagedBy(mgr).
		For(&vmafxv1.VmafxModelTraining{}).
		Complete(r)
}
