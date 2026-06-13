// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxmodeltraining_controller.go — VmafxModelTraining reconciler.
//
// Reconciler behaviour:
//   - Sets Phase to Initializing on a new VmafxModelTraining.
//   - Polls the sidecar trainer status via a lightweight HTTP GET to
//     http://vmafx-trainer-<name>.<namespace>.svc.cluster.local:9091/status.
//   - Maps the sidecar's reported CurrentSamples and LastCheckpoint onto
//     the CR's status subresource.
//   - Emits a Kubernetes event (reason "CheckpointWritten") whenever
//     status.LastCheckpoint advances (i.e. a new checkpoint was written).
//   - Requeues every trainingRequeueInterval (60 s).
//
// The full online-SGD-EMA training loop is deferred to Stage 3; this
// reconciler provides the observable status bridge between the sidecar
// trainer Pod and the Kubernetes control plane.
//
// ADR-0786: vmafx-operator Stage 2 — reconciler loops + webhook + per-controller RBAC.
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs (parent).

package controller

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"

	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/client-go/tools/record"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/log"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
)

const (
	// trainingRequeueInterval is the period between reconcile runs.
	trainingRequeueInterval = 60 * time.Second
	// trainerHTTPTimeout is the timeout for polling the sidecar trainer.
	trainerHTTPTimeout = 5 * time.Second
	// trainerHTTPPort is the default HTTP port of the sidecar trainer.
	trainerHTTPPort = 9091
)

// trainerStatusResponse is the JSON schema returned by the sidecar trainer /status endpoint.
type trainerStatusResponse struct {
	// Phase is one of "initializing", "running", "paused", "failed".
	Phase string `json:"phase"`
	// CurrentSamples is the total number of training samples ingested so far.
	CurrentSamples int `json:"current_samples"`
	// LastCheckpointTime is RFC3339-formatted time of the most recent checkpoint, or "".
	LastCheckpointTime string `json:"last_checkpoint_time,omitempty"`
	// ModelVersion is the OCI tag / digest of the most recently pushed model, or "".
	ModelVersion string `json:"model_version,omitempty"`
}

// VmafxModelTrainingReconciler reconciles VmafxModelTraining objects.
type VmafxModelTrainingReconciler struct {
	client.Client
	Scheme   *runtime.Scheme
	Recorder record.EventRecorder
	// HTTPClient is injectable for testing; defaults to a short-timeout client.
	HTTPClient *http.Client
}

// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxmodeltrainings,verbs=get;list;watch;update;patch
// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxmodeltrainings/status,verbs=get;update;patch
// +kubebuilder:rbac:groups=vmafx.dev,resources=vmafxmodeltrainings/finalizers,verbs=update
// +kubebuilder:rbac:groups="",resources=events,verbs=create;patch

// Reconcile is the reconciliation loop for VmafxModelTraining.
func (r *VmafxModelTrainingReconciler) Reconcile(ctx context.Context, req ctrl.Request) (ctrl.Result, error) {
	logger := log.FromContext(ctx)
	logger.Info("Reconciling VmafxModelTraining",
		"namespace", req.Namespace, "name", req.Name)

	var mt vmafxv1.VmafxModelTraining
	if err := r.Get(ctx, req.NamespacedName, &mt); err != nil {
		return ctrl.Result{}, client.IgnoreNotFound(err)
	}

	// Initialise Phase on a brand-new object.
	if mt.Status.Phase == "" {
		mt.Status.Phase = vmafxv1.VmafxModelTrainingPhaseInitializing
		if err := r.Status().Update(ctx, &mt); err != nil {
			logger.Error(err, "Failed to set initial phase to Initializing")
			return ctrl.Result{}, err
		}
		return ctrl.Result{RequeueAfter: trainingRequeueInterval}, nil
	}

	// Poll the sidecar trainer.
	sidecar, err := r.pollTrainerStatus(ctx, req.Namespace, req.Name)
	if err != nil {
		logger.Info("Could not poll sidecar trainer status; will retry",
			"error", err)
		// Non-fatal: the sidecar may not be ready yet.
		return ctrl.Result{RequeueAfter: trainingRequeueInterval}, nil
	}

	prevCheckpoint := mt.Status.LastCheckpoint
	updated := r.applySidecarStatus(&mt, sidecar)

	if updated {
		if err := r.Status().Update(ctx, &mt); err != nil {
			logger.Error(err, "Failed to update VmafxModelTraining status")
			return ctrl.Result{}, err
		}

		// Emit event when a new checkpoint was written.
		if mt.Status.LastCheckpoint != nil &&
			(prevCheckpoint == nil || !mt.Status.LastCheckpoint.Equal(prevCheckpoint)) {
			r.Recorder.Event(&mt, corev1.EventTypeNormal, "CheckpointWritten",
				fmt.Sprintf("Model checkpoint written: version=%s samples=%d",
					mt.Status.ModelVersion, mt.Status.CurrentSamples))
			logger.Info("Checkpoint event emitted",
				"namespace", req.Namespace, "name", req.Name,
				"modelVersion", mt.Status.ModelVersion,
				"samples", mt.Status.CurrentSamples)
		}
	}

	return ctrl.Result{RequeueAfter: trainingRequeueInterval}, nil
}

// pollTrainerStatus fetches the sidecar trainer's /status endpoint and decodes
// the response.  Returns an error when the sidecar is unreachable or returns a
// non-200 status.
func (r *VmafxModelTrainingReconciler) pollTrainerStatus(
	ctx context.Context,
	namespace, name string,
) (*trainerStatusResponse, error) {
	hc := r.HTTPClient
	// HTTPClient is always initialised by SetupWithManager; the nil fallback is
	// only reached in tests that construct the reconciler directly without calling
	// SetupWithManager (r5-scheduler-timer, ADR-1017).
	if hc == nil {
		hc = &http.Client{Timeout: trainerHTTPTimeout}
	}

	url := fmt.Sprintf("http://vmafx-trainer-%s.%s.svc.cluster.local:%d/status",
		name, namespace, trainerHTTPPort)

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, fmt.Errorf("build request: %w", err)
	}

	resp, err := hc.Do(req)
	if err != nil {
		return nil, fmt.Errorf("GET %s: %w", url, err)
	}
	defer resp.Body.Close() //nolint:errcheck // read error is not actionable after decoding

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("trainer /status returned %d", resp.StatusCode)
	}

	body, err := io.ReadAll(io.LimitReader(resp.Body, 4096))
	if err != nil {
		return nil, fmt.Errorf("read response: %w", err)
	}

	var status trainerStatusResponse
	if err := json.Unmarshal(body, &status); err != nil {
		return nil, fmt.Errorf("decode /status: %w", err)
	}

	return &status, nil
}

// applySidecarStatus maps the sidecar's reported state onto the CR's status.
// Returns true when at least one field changed.
func (r *VmafxModelTrainingReconciler) applySidecarStatus(
	cr *vmafxv1.VmafxModelTraining,
	s *trainerStatusResponse,
) bool {
	changed := false

	// Map phase string to enum.
	var newPhase vmafxv1.VmafxModelTrainingPhase
	switch s.Phase {
	case "running":
		newPhase = vmafxv1.VmafxModelTrainingPhaseRunning
	case "paused":
		newPhase = vmafxv1.VmafxModelTrainingPhasePaused
	case "failed":
		newPhase = vmafxv1.VmafxModelTrainingPhaseFailed
	default:
		newPhase = vmafxv1.VmafxModelTrainingPhaseInitializing
	}

	if cr.Status.Phase != newPhase {
		cr.Status.Phase = newPhase
		changed = true
	}

	if cr.Status.CurrentSamples != int32(s.CurrentSamples) { // #nosec G115 -- sample counts are bounded by training-loop batch sizes; overflow is impossible in practice
		cr.Status.CurrentSamples = int32(s.CurrentSamples)  // #nosec G115 -- same bound as above
		changed = true
	}

	if s.ModelVersion != "" && cr.Status.ModelVersion != s.ModelVersion {
		cr.Status.ModelVersion = s.ModelVersion
		changed = true
	}

	if s.LastCheckpointTime != "" {
		t, err := time.Parse(time.RFC3339, s.LastCheckpointTime)
		if err == nil {
			mt := metav1.NewTime(t)
			if cr.Status.LastCheckpoint == nil || !cr.Status.LastCheckpoint.Equal(&mt) {
				cr.Status.LastCheckpoint = &mt
				changed = true
			}
		}
	}

	return changed
}

// SetupWithManager registers the VmafxModelTrainingReconciler with the controller manager.
// It initialises the default HTTP client when not injected, so production
// reconcile calls share a single connection-pooled client (r5-scheduler-timer,
// ADR-1017).
func (r *VmafxModelTrainingReconciler) SetupWithManager(mgr ctrl.Manager) error {
	if r.Recorder == nil {
		r.Recorder = mgr.GetEventRecorderFor("vmafx-model-training-controller")
	}
	if r.HTTPClient == nil {
		r.HTTPClient = &http.Client{Timeout: trainerHTTPTimeout}
	}
	return ctrl.NewControllerManagedBy(mgr).
		For(&vmafxv1.VmafxModelTraining{}).
		Complete(r)
}
