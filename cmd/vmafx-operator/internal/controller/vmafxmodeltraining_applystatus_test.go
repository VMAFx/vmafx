// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxmodeltraining_applystatus_test.go
// — table-driven unit tests for VmafxModelTrainingReconciler.applySidecarStatus
// and pollTrainerStatus.
//
// applySidecarStatus is a pure mapping function and needs no k8s API.
// pollTrainerStatus is tested via an httptest.Server to avoid real network calls.
//
// ADR-0786: vmafx-operator Stage 2 — reconciler loops.

package controller

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
)

// TestApplySidecarStatus_PhaseMapping verifies that each sidecar phase string
// is mapped to the correct VmafxModelTrainingPhase enum value.
func TestApplySidecarStatus_PhaseMapping(t *testing.T) {
	t.Parallel()
	tests := []struct {
		sidecarPhase string
		wantPhase    vmafxv1.VmafxModelTrainingPhase
	}{
		{"running", vmafxv1.VmafxModelTrainingPhaseRunning},
		{"paused", vmafxv1.VmafxModelTrainingPhasePaused},
		{"failed", vmafxv1.VmafxModelTrainingPhaseFailed},
		{"", vmafxv1.VmafxModelTrainingPhaseInitializing},
		{"unknown", vmafxv1.VmafxModelTrainingPhaseInitializing},
	}

	r := &VmafxModelTrainingReconciler{}
	for _, tt := range tests {
		tt := tt
		t.Run("phase="+tt.sidecarPhase, func(t *testing.T) {
			t.Parallel()
			cr := &vmafxv1.VmafxModelTraining{}
			s := &trainerStatusResponse{Phase: tt.sidecarPhase}
			changed := r.applySidecarStatus(cr, s)
			if !changed {
				t.Error("expected changed=true on first application")
			}
			if cr.Status.Phase != tt.wantPhase {
				t.Errorf("phase = %q, want %q", cr.Status.Phase, tt.wantPhase)
			}
		})
	}
}

// TestApplySidecarStatus_Idempotent verifies a second application with the same
// sidecar state does NOT flip changed=true.
func TestApplySidecarStatus_Idempotent(t *testing.T) {
	t.Parallel()
	r := &VmafxModelTrainingReconciler{}
	cr := &vmafxv1.VmafxModelTraining{}
	s := &trainerStatusResponse{Phase: "running", CurrentSamples: 100}

	if !r.applySidecarStatus(cr, s) {
		t.Fatal("first: expected changed=true")
	}
	if r.applySidecarStatus(cr, s) {
		t.Error("second: expected changed=false (idempotent)")
	}
}

// TestApplySidecarStatus_CurrentSamplesUpdated verifies that CurrentSamples
// is written to the CR status with the correct int32 conversion.
func TestApplySidecarStatus_CurrentSamplesUpdated(t *testing.T) {
	t.Parallel()
	r := &VmafxModelTrainingReconciler{}
	cr := &vmafxv1.VmafxModelTraining{}
	s := &trainerStatusResponse{Phase: "running", CurrentSamples: 42000}
	r.applySidecarStatus(cr, s)
	if cr.Status.CurrentSamples != 42000 {
		t.Errorf("CurrentSamples = %d, want 42000", cr.Status.CurrentSamples)
	}
}

// TestApplySidecarStatus_ModelVersionPropagated verifies that a non-empty
// ModelVersion is written to the CR status.
func TestApplySidecarStatus_ModelVersionPropagated(t *testing.T) {
	t.Parallel()
	r := &VmafxModelTrainingReconciler{}
	cr := &vmafxv1.VmafxModelTraining{}
	s := &trainerStatusResponse{
		Phase:        "running",
		ModelVersion: "sha256:abc123",
	}
	r.applySidecarStatus(cr, s)
	if cr.Status.ModelVersion != "sha256:abc123" {
		t.Errorf("ModelVersion = %q, want %q", cr.Status.ModelVersion, "sha256:abc123")
	}
}

// TestApplySidecarStatus_LastCheckpointParsed verifies that a valid RFC3339
// LastCheckpointTime is parsed and stored in status.
func TestApplySidecarStatus_LastCheckpointParsed(t *testing.T) {
	t.Parallel()
	r := &VmafxModelTrainingReconciler{}
	cr := &vmafxv1.VmafxModelTraining{}
	s := &trainerStatusResponse{
		Phase:              "running",
		LastCheckpointTime: "2026-06-03T12:00:00Z",
	}
	changed := r.applySidecarStatus(cr, s)
	if !changed {
		t.Fatal("expected changed=true when LastCheckpointTime is set")
	}
	if cr.Status.LastCheckpoint == nil {
		t.Fatal("LastCheckpoint should be set after parsing a valid time")
	}
}

// TestApplySidecarStatus_InvalidCheckpointIgnored verifies that an unparseable
// LastCheckpointTime does not change the CR status and does not panic.
func TestApplySidecarStatus_InvalidCheckpointIgnored(t *testing.T) {
	t.Parallel()
	r := &VmafxModelTrainingReconciler{}
	cr := &vmafxv1.VmafxModelTraining{}
	s := &trainerStatusResponse{
		Phase:              "running",
		LastCheckpointTime: "not-a-date",
	}
	// applySidecarStatus returns true for the phase change, but the checkpoint
	// parse error is silently swallowed — verify no panic.
	r.applySidecarStatus(cr, s)
	if cr.Status.LastCheckpoint != nil {
		t.Error("LastCheckpoint should remain nil after an invalid time string")
	}
}

// TestPollTrainerStatus_OK verifies that a 200 response with a valid JSON body
// is decoded correctly.
func TestPollTrainerStatus_OK(t *testing.T) {
	t.Parallel()
	payload := trainerStatusResponse{
		Phase:          "running",
		CurrentSamples: 500,
		ModelVersion:   "v1.2.3",
	}
	b, _ := json.Marshal(payload)

	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(b)
	}))
	defer ts.Close()

	r := &VmafxModelTrainingReconciler{}
	// Override the URL by setting HTTPClient to connect to the test server.
	// pollTrainerStatus builds the URL from the namespace+name, so we inject
	// via the ControllerHTTPAddr analogue — but VmafxModelTrainingReconciler
	// doesn't have that field. Instead we override the HTTP client Transport.
	r.HTTPClient = ts.Client()

	// pollTrainerStatus builds URL as:
	//   http://vmafx-trainer-<name>.<namespace>.svc.cluster.local:<port>/status
	// For the unit test we replace the whole URL by injecting a custom RoundTripper.
	r.HTTPClient = &http.Client{
		Transport: &redirectRoundTripper{target: ts.URL + "/status"},
	}

	ctx := t.Context()
	status, err := r.pollTrainerStatus(ctx, "default", "my-trainer")
	if err != nil {
		t.Fatalf("pollTrainerStatus: %v", err)
	}
	if status.Phase != "running" {
		t.Errorf("Phase = %q, want %q", status.Phase, "running")
	}
	if status.CurrentSamples != 500 {
		t.Errorf("CurrentSamples = %d, want 500", status.CurrentSamples)
	}
}

// TestPollTrainerStatus_Non200 verifies that a non-200 response returns an error.
func TestPollTrainerStatus_Non200(t *testing.T) {
	t.Parallel()
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer ts.Close()

	r := &VmafxModelTrainingReconciler{
		HTTPClient: &http.Client{
			Transport: &redirectRoundTripper{target: ts.URL + "/status"},
		},
	}
	_, err := r.pollTrainerStatus(t.Context(), "default", "my-trainer")
	if err == nil {
		t.Error("expected error on non-200, got nil")
	}
}

// redirectRoundTripper replaces the request URL with a fixed target.
// Used to redirect pollTrainerStatus calls from the in-cluster DNS URL to the
// httptest.Server without requiring DNS resolution in unit tests.
type redirectRoundTripper struct {
	target string
}

func (rr *redirectRoundTripper) RoundTrip(req *http.Request) (*http.Response, error) {
	newReq, err := http.NewRequestWithContext(req.Context(), req.Method, rr.target, req.Body)
	if err != nil {
		return nil, err
	}
	return http.DefaultTransport.RoundTrip(newReq)
}
