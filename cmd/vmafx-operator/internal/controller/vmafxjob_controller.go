// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxjob_controller.go — VmafxJob reconciler (Stage 2).
//
// Reconciler behaviour:
//   - Initialises Phase to Pending on a new VmafxJob (no ControllerJobID yet).
//   - When ControllerJobID is set and Phase is Pending, polls the vmafx-controller
//     gRPC GetJob endpoint and maps the remote status to the CR phase:
//       PENDING   → Pending
//       RUNNING   → Running (sets StartedAt)
//       COMPLETED → Succeeded (sets Score, FinishedAt)
//       FAILED    → Failed (sets Message, FinishedAt)
//       CANCELLED → Failed with Message="cancelled"
//   - Running jobs are requeued after jobPollInterval.
//   - Terminal phases (Succeeded / Failed) are not requeued.
//
// The gRPC target is resolved from the env var VMAFX_CONTROLLER_GRPC_ADDR
// (default "vmafx-controller.<namespace>.svc.cluster.local:9090").
//
// ADR-0786: vmafx-operator Stage 2 — reconciler loops + webhook + per-controller RBAC.
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs (parent).
// ADR-0711: vmafx-controller Phase 4b.1 (gRPC service definition).

package controller

import (
	"context"
	"fmt"
	"os"
	"time"

	grpcmod "github.com/golusoris/golusoris/grpc"
	"google.golang.org/grpc/codes"
	grpcstatus "google.golang.org/grpc/status"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/log"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

const (
	// jobPollInterval is the requeue period for jobs in a non-terminal phase.
	jobPollInterval = 10 * time.Second
	// grpcDialTimeout is the maximum time allowed to establish the gRPC connection.
	grpcDialTimeout = 5 * time.Second
)

// VmafxJobReconciler reconciles VmafxJob objects.
type VmafxJobReconciler struct {
	client.Client
	Scheme *runtime.Scheme
	// ControllerAddr overrides VMAFX_CONTROLLER_GRPC_ADDR when set (useful in tests).
	ControllerAddr string
}

// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxjobs,verbs=get;list;watch;update;patch
// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxjobs/status,verbs=get;update;patch
// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxjobs/finalizers,verbs=update

// Reconcile is the reconciliation loop for VmafxJob.
func (r *VmafxJobReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
	logger := log.FromContext(ctx)
	logger.Info("Reconciling VmafxJob", "namespace", req.Namespace, "name", req.Name)

	var job vmafxv1.VmafxJob
	if err := r.Get(ctx, req.NamespacedName, &job); err != nil {
		return ctrl.Result{}, client.IgnoreNotFound(err)
	}

	// Initialise Phase on a brand-new object.
	if job.Status.Phase == "" {
		job.Status.Phase = vmafxv1.VmafxJobPhasePending
		if err := r.Status().Update(ctx, &job); err != nil {
			logger.Error(err, "Failed to set initial phase to Pending")
			return ctrl.Result{}, err
		}
		return ctrl.Result{RequeueAfter: jobPollInterval}, nil
	}

	// Terminal phases need no further action.
	if job.Status.Phase == vmafxv1.VmafxJobPhaseSucceeded ||
		job.Status.Phase == vmafxv1.VmafxJobPhaseFailed {
		return ctrl.Result{}, nil
	}

	// If no controller job ID is assigned yet, wait for the external scheduler.
	if job.Status.ControllerJobID == "" {
		logger.Info("No ControllerJobID yet — waiting for scheduler",
			"phase", job.Status.Phase)
		return ctrl.Result{RequeueAfter: jobPollInterval}, nil
	}

	// Poll the gRPC controller for the remote job state.
	remoteJob, err := r.getRemoteJob(ctx, req.Namespace, job.Status.ControllerJobID)
	if err != nil {
		logger.Error(err, "Failed to poll controller for job status",
			"controllerJobID", job.Status.ControllerJobID)
		return ctrl.Result{RequeueAfter: jobPollInterval}, nil
	}

	updated := r.applyRemoteStatus(&job, remoteJob)
	if !updated {
		return ctrl.Result{RequeueAfter: jobPollInterval}, nil
	}

	if err := r.Status().Update(ctx, &job); err != nil {
		logger.Error(err, "Failed to update VmafxJob status from remote")
		return ctrl.Result{}, err
	}

	logger.Info("VmafxJob status updated",
		"phase", job.Status.Phase,
		"score", job.Status.Score,
		"controllerJobID", job.Status.ControllerJobID)

	// Terminal phase reached — no requeue.
	if job.Status.Phase == vmafxv1.VmafxJobPhaseSucceeded ||
		job.Status.Phase == vmafxv1.VmafxJobPhaseFailed {
		return ctrl.Result{}, nil
	}

	return ctrl.Result{RequeueAfter: jobPollInterval}, nil
}

// getRemoteJob dials the vmafx-controller and retrieves the current job state.
func (r *VmafxJobReconciler) getRemoteJob(
	ctx context.Context,
	namespace, jobID string,
) (*controllerv1.Job, error) {
	addr := r.resolveControllerAddr(namespace)

	dialCtx, cancel := context.WithTimeout(ctx, grpcDialTimeout)
	defer cancel()

	// The dial goes through golusoris's ConnFactory (grpc.NewClient underneath,
	// so it never blocks: controller-runtime keeps scheduling other objects
	// while the connection resolves in the background) so the outgoing GetJob
	// carries the otelgrpc client handler and therefore a W3C traceparent —
	// the operator→controller hop joins the same trace as the controller's
	// server span (ADR-0782, ADR-1095, ADR-1119). Transport credentials are the
	// factory's insecure default, as before. The per-call overhead from
	// creating a new connection each Reconcile is accepted; a shared cached
	// conn is out-of-scope for this PR (r5-scheduler-timer, ADR-1017).
	conn, err := grpcmod.NewConnFactory().Dial(dialCtx, addr)
	if err != nil {
		return nil, fmt.Errorf("dial %s: %w", addr, err)
	}
	defer conn.Close() //nolint:errcheck // connection close errors are not actionable

	c := controllerv1.NewVmafxControllerClient(conn)
	// Pass dialCtx so the RPC inherits the same deadline as the dial; without
	// this the parent ctx (no deadline) would allow GetJob to block indefinitely
	// (r5-scheduler-timer finding :153, ADR-1017).
	job, err := c.GetJob(dialCtx, &controllerv1.GetJobRequest{JobId: jobID})
	if err != nil {
		if s, ok := grpcstatus.FromError(err); ok && s.Code() == codes.NotFound {
			return nil, fmt.Errorf("job %s not found on controller", jobID)
		}
		return nil, fmt.Errorf("GetJob %s: %w", jobID, err)
	}
	return job, nil
}

// applyRemoteStatus maps the controller job status onto the CR status.
// Returns true when any field changed.
func (r *VmafxJobReconciler) applyRemoteStatus(
	cr *vmafxv1.VmafxJob,
	remote *controllerv1.Job,
) bool {
	changed := false
	now := metav1.NewTime(time.Now())

	switch remote.GetStatus() {
	case controllerv1.JobStatus_PENDING:
		if cr.Status.Phase != vmafxv1.VmafxJobPhasePending {
			cr.Status.Phase = vmafxv1.VmafxJobPhasePending
			changed = true
		}

	case controllerv1.JobStatus_RUNNING:
		if cr.Status.Phase != vmafxv1.VmafxJobPhaseRunning {
			cr.Status.Phase = vmafxv1.VmafxJobPhaseRunning
			cr.Status.StartedAt = &now
			changed = true
		}

	case controllerv1.JobStatus_COMPLETED:
		if cr.Status.Phase != vmafxv1.VmafxJobPhaseSucceeded {
			cr.Status.Phase = vmafxv1.VmafxJobPhaseSucceeded
			cr.Status.Score = remote.GetFinalScore()
			cr.Status.FinishedAt = &now
			changed = true
		}

	case controllerv1.JobStatus_FAILED:
		if cr.Status.Phase != vmafxv1.VmafxJobPhaseFailed {
			cr.Status.Phase = vmafxv1.VmafxJobPhaseFailed
			cr.Status.Message = remote.GetError()
			cr.Status.FinishedAt = &now
			changed = true
		}

	case controllerv1.JobStatus_CANCELLED:
		if cr.Status.Phase != vmafxv1.VmafxJobPhaseFailed {
			cr.Status.Phase = vmafxv1.VmafxJobPhaseFailed
			cr.Status.Message = "cancelled by controller"
			cr.Status.FinishedAt = &now
			changed = true
		}
	}

	// Sync AssignedNode.
	if remote.GetAssignedNode() != "" && cr.Status.AssignedNode != remote.GetAssignedNode() {
		cr.Status.AssignedNode = remote.GetAssignedNode()
		changed = true
	}

	return changed
}

// resolveControllerAddr returns the gRPC address of the vmafx-controller.
func (r *VmafxJobReconciler) resolveControllerAddr(namespace string) string {
	if r.ControllerAddr != "" {
		return r.ControllerAddr
	}
	if v := os.Getenv("VMAFX_CONTROLLER_GRPC_ADDR"); v != "" {
		return v
	}
	return fmt.Sprintf("vmafx-controller.%s.svc.cluster.local:9090", namespace)
}

// SetupWithManager registers the VmafxJobReconciler with the controller manager.
func (r *VmafxJobReconciler) SetupWithManager(mgr ctrl.Manager) error {
	return ctrl.NewControllerManagedBy(mgr).
		For(&vmafxv1.VmafxJob{}).
		Complete(r)
}
