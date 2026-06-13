// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// api/vmafx/v1/vmafxnode_types.go — VmafxNode CRD type definitions.
//
// VmafxNode represents a compute node that can execute VmafxJobs.  Each
// vmafx-node agent registers a corresponding VmafxNode CR on startup.
// The operator reconciles health status by polling the node's /healthz
// endpoint and updating Healthy + LastHeartbeat in the status subresource.
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.
// ADR-0709: VMAFX Phase 4b distributed platform (parent).

package v1

import (
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
)

// VmafxNodeSpec defines the desired state of a VmafxNode.
type VmafxNodeSpec struct {
	// GPUVendor identifies the GPU vendor on this node.
	// +kubebuilder:validation:Enum=nvidia;amd;intel;cpu
	GPUVendor string `json:"gpuVendor"`

	// Capacity is the maximum number of concurrent VmafxJobs this node can execute.
	// Defaults to 1 when omitted.
	// +kubebuilder:validation:Minimum=1
	// +kubebuilder:default:=1
	// +optional
	Capacity int32 `json:"capacity,omitempty"`

	// Image is the container image used when the operator creates worker Pods for this node.
	// Defaults to the operator's global workerImage when omitted.
	// +optional
	Image string `json:"image,omitempty"`
}

// VmafxNodeStatus defines the observed state of a VmafxNode.
type VmafxNodeStatus struct {
	// Healthy is true when the node's /healthz endpoint last returned HTTP 200.
	Healthy bool `json:"healthy"`

	// LastHeartbeat records the last time the node's health was confirmed.
	// +optional
	LastHeartbeat *metav1.Time `json:"lastHeartbeat,omitempty"`

	// AssignedJobs is the number of VmafxJobs currently in Running phase on this node.
	AssignedJobs int32 `json:"assignedJobs"`

	// DetectedDevice is the device identifier reported by the node's /healthz probe
	// (e.g., "NVIDIA RTX 4090", "Intel Arc A770").
	// +optional
	DetectedDevice string `json:"detectedDevice,omitempty"`
}

// +kubebuilder:object:root=true
// +kubebuilder:subresource:status
// +kubebuilder:resource:shortName=vmnode
// +kubebuilder:printcolumn:name="Vendor",type="string",JSONPath=".spec.gpuVendor"
// +kubebuilder:printcolumn:name="Healthy",type="boolean",JSONPath=".status.healthy"
// +kubebuilder:printcolumn:name="Jobs",type="integer",JSONPath=".status.assignedJobs"
// +kubebuilder:printcolumn:name="Device",type="string",JSONPath=".status.detectedDevice"
// +kubebuilder:printcolumn:name="Age",type="date",JSONPath=".metadata.creationTimestamp"

// VmafxNode is the Schema for the vmafxnodes API.
type VmafxNode struct {
	metav1.TypeMeta   `json:",inline"`
	metav1.ObjectMeta `json:"metadata,omitempty"`

	Spec   VmafxNodeSpec   `json:"spec,omitempty"`
	Status VmafxNodeStatus `json:"status,omitempty"`
}

// +kubebuilder:object:root=true

// VmafxNodeList contains a list of VmafxNode.
type VmafxNodeList struct {
	metav1.TypeMeta `json:",inline"`
	metav1.ListMeta `json:"metadata,omitempty"`
	Items           []VmafxNode `json:"items"`
}

func init() {
	SchemeBuilder.Register(&VmafxNode{}, &VmafxNodeList{})
}
