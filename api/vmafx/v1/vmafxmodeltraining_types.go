// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// api/vmafx/v1/vmafxmodeltraining_types.go — VmafxModelTraining CRD type definitions.
//
// VmafxModelTraining drives the sidecar online-learning pipeline described in
// Research #30 and ADR-0709 Phase 4b.  The operator's stub reconciler logs
// and requeues every minute; the full training-loop controller arrives in
// Stage 2 PRs.
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.
// ADR-0709: VMAFX Phase 4b distributed platform (parent).

package v1

import (
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
)

// VmafxModelTrainingPhase enumerates the lifecycle phases of a VmafxModelTraining run.
type VmafxModelTrainingPhase string

const (
	// VmafxModelTrainingPhaseInitializing means the training run is being set up.
	VmafxModelTrainingPhaseInitializing VmafxModelTrainingPhase = "Initializing"
	// VmafxModelTrainingPhaseRunning means the training loop is active and collecting samples.
	VmafxModelTrainingPhaseRunning VmafxModelTrainingPhase = "Running"
	// VmafxModelTrainingPhasePaused means training has been intentionally paused.
	VmafxModelTrainingPhasePaused VmafxModelTrainingPhase = "Paused"
	// VmafxModelTrainingPhaseFailed means training failed; see the controller logs for details.
	VmafxModelTrainingPhaseFailed VmafxModelTrainingPhase = "Failed"
)

// VmafxModelTrainingDataSource describes where training samples originate.
type VmafxModelTrainingDataSource struct {
	// NodeSelector constrains which VmafxNodes provide training samples.
	// An empty selector matches all nodes.
	// +optional
	NodeSelector map[string]string `json:"nodeSelector,omitempty"`
}

// VmafxModelTrainingCheckpoint configures periodic model checkpointing.
type VmafxModelTrainingCheckpoint struct {
	// Interval is the minimum wall-clock duration between checkpoints (e.g. "10m").
	// Defaults to "10m" when omitted.
	// +kubebuilder:default:="10m"
	// +optional
	Interval string `json:"interval,omitempty"`

	// MinSamples is the minimum number of new training samples required before
	// a checkpoint is written.  Defaults to 1000 when omitted.
	// +kubebuilder:validation:Minimum=1
	// +kubebuilder:default:=1000
	// +optional
	MinSamples int32 `json:"minSamples,omitempty"`
}

// VmafxModelTrainingSpec defines the desired state of a VmafxModelTraining run.
type VmafxModelTrainingSpec struct {
	// BaseModel is the name of the seed model (e.g. "vmaf_v0.6.1").
	// +kubebuilder:validation:MinLength=1
	BaseModel string `json:"baseModel"`

	// DataSource configures where training samples come from.
	DataSource VmafxModelTrainingDataSource `json:"dataSource"`

	// Algorithm selects the online learning algorithm.
	// Only "online-sgd-ema" is supported in Stage 1.
	// +kubebuilder:validation:Enum=online-sgd-ema
	Algorithm string `json:"algorithm"`

	// Checkpoint configures periodic model checkpointing.
	// +optional
	Checkpoint VmafxModelTrainingCheckpoint `json:"checkpoint,omitempty"`

	// OutputRegistry is the OCI registry URI where trained model artefacts are pushed.
	// +kubebuilder:validation:MinLength=1
	OutputRegistry string `json:"outputRegistry"`
}

// VmafxModelTrainingStatus defines the observed state of a VmafxModelTraining run.
type VmafxModelTrainingStatus struct {
	// CurrentSamples is the total number of training samples ingested so far.
	CurrentSamples int32 `json:"currentSamples"`

	// LastCheckpoint records when the most recent model checkpoint was written.
	// +optional
	LastCheckpoint *metav1.Time `json:"lastCheckpoint,omitempty"`

	// ModelVersion is the tag or digest of the most recently pushed model artefact.
	// +optional
	ModelVersion string `json:"modelVersion,omitempty"`

	// Phase is the current lifecycle phase of the training run.
	// +kubebuilder:validation:Enum=Initializing;Running;Paused;Failed
	Phase VmafxModelTrainingPhase `json:"phase"`
}

// +kubebuilder:object:root=true
// +kubebuilder:subresource:status
// +kubebuilder:resource:shortName=vmtrain
// +kubebuilder:printcolumn:name="Phase",type="string",JSONPath=".status.phase"
// +kubebuilder:printcolumn:name="Samples",type="integer",JSONPath=".status.currentSamples"
// +kubebuilder:printcolumn:name="ModelVersion",type="string",JSONPath=".status.modelVersion"
// +kubebuilder:printcolumn:name="Age",type="date",JSONPath=".metadata.creationTimestamp"

// VmafxModelTraining is the Schema for the vmafxmodeltrainings API.
type VmafxModelTraining struct {
	metav1.TypeMeta   `json:",inline"`
	metav1.ObjectMeta `json:"metadata,omitempty"`

	Spec   VmafxModelTrainingSpec   `json:"spec,omitempty"`
	Status VmafxModelTrainingStatus `json:"status,omitempty"`
}

// +kubebuilder:object:root=true

// VmafxModelTrainingList contains a list of VmafxModelTraining.
type VmafxModelTrainingList struct {
	metav1.TypeMeta `json:",inline"`
	metav1.ListMeta `json:"metadata,omitempty"`
	Items           []VmafxModelTraining `json:"items"`
}

func init() {
	SchemeBuilder.Register(&VmafxModelTraining{}, &VmafxModelTrainingList{})
}
