// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// api/vmafx/v1/vmafxjob_types.go — VmafxJob CRD type definitions.
//
// VmafxJob represents a single video-quality scoring job: one reference/distorted
// pair scored against a chosen model on a chosen GPU backend.  The operator
// creates a Pod for each VmafxJob and propagates terminal status back to the
// CR's Status subresource.
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.
// ADR-0709: VMAFX Phase 4b distributed platform (parent).

package v1

import (
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
)

// VmafxJobPhase enumerates the lifecycle phases of a VmafxJob.
type VmafxJobPhase string

const (
	// VmafxJobPhasePending means the job has been accepted but not yet assigned to a node.
	VmafxJobPhasePending VmafxJobPhase = "Pending"
	// VmafxJobPhaseRunning means the job has been dispatched to a VmafxNode and a Pod is active.
	VmafxJobPhaseRunning VmafxJobPhase = "Running"
	// VmafxJobPhaseSucceeded means the job completed successfully and a score is available.
	VmafxJobPhaseSucceeded VmafxJobPhase = "Succeeded"
	// VmafxJobPhaseFailed means the job terminated with an error; see Message for details.
	VmafxJobPhaseFailed VmafxJobPhase = "Failed"
)

// VmafxJobSpec defines the desired state of a VmafxJob.
type VmafxJobSpec struct {
	// Reference is the URI of the reference video.
	// Supported schemes: file://, s3://, rclone://.
	// +kubebuilder:validation:MinLength=1
	Reference string `json:"reference"`

	// Distorted is the URI of the distorted video to compare against Reference.
	// +kubebuilder:validation:MinLength=1
	Distorted string `json:"distorted"`

	// Model is the VMAF model to use (e.g. "vmaf_v0.6.1").
	// Defaults to the node's default model when omitted.
	// +optional
	Model string `json:"model,omitempty"`

	// Backend selects the compute backend: cpu, cuda, sycl, hip, or metal.
	// Defaults to cpu when omitted.
	// +kubebuilder:validation:Enum=cpu;cuda;sycl;hip;metal
	// +optional
	Backend string `json:"backend,omitempty"`

	// Priority is a non-negative scheduling priority; higher values are dispatched first.
	// +kubebuilder:validation:Minimum=0
	// +optional
	Priority int `json:"priority,omitempty"`
}

// VmafxJobStatus defines the observed state of a VmafxJob.
type VmafxJobStatus struct {
	// Phase is the current lifecycle phase of the job.
	// +kubebuilder:validation:Enum=Pending;Running;Succeeded;Failed
	Phase VmafxJobPhase `json:"phase"`

	// Score is the VMAF score produced by the job.
	// Set only when Phase == Succeeded.
	// +optional
	Score float64 `json:"score,omitempty"`

	// AssignedNode is the name of the VmafxNode that is executing (or executed) this job.
	// +optional
	AssignedNode string `json:"assignedNode,omitempty"`

	// ControllerJobID is the UUID assigned by the vmafx-controller when the job is
	// dispatched.  The reconciler uses this to poll GetJob on the controller's gRPC API.
	// Set by the external scheduler; empty until the job is accepted by a controller.
	// +optional
	ControllerJobID string `json:"controllerJobID,omitempty"`

	// StartedAt records when the job Pod began running.
	// +optional
	StartedAt *metav1.Time `json:"startedAt,omitempty"`

	// FinishedAt records when the job reached a terminal phase (Succeeded or Failed).
	// +optional
	FinishedAt *metav1.Time `json:"finishedAt,omitempty"`

	// Message is a human-readable explanation of the current phase; populated on failure.
	// +optional
	Message string `json:"message,omitempty"`
}

// +kubebuilder:object:root=true
// +kubebuilder:subresource:status
// +kubebuilder:resource:shortName=vmjob
// +kubebuilder:printcolumn:name="Phase",type="string",JSONPath=".status.phase"
// +kubebuilder:printcolumn:name="Score",type="number",JSONPath=".status.score"
// +kubebuilder:printcolumn:name="Node",type="string",JSONPath=".status.assignedNode"
// +kubebuilder:printcolumn:name="Age",type="date",JSONPath=".metadata.creationTimestamp"

// VmafxJob is the Schema for the vmafxjobs API.
type VmafxJob struct {
	metav1.TypeMeta   `json:",inline"`
	metav1.ObjectMeta `json:"metadata,omitempty"`

	Spec   VmafxJobSpec   `json:"spec,omitempty"`
	Status VmafxJobStatus `json:"status,omitempty"`
}

// +kubebuilder:object:root=true

// VmafxJobList contains a list of VmafxJob.
type VmafxJobList struct {
	metav1.TypeMeta `json:",inline"`
	metav1.ListMeta `json:"metadata,omitempty"`
	Items           []VmafxJob `json:"items"`
}

func init() {
	SchemeBuilder.Register(&VmafxJob{}, &VmafxJobList{})
}
