// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// api/vmafx/v1/groupversion_info.go — API group / version registration.
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.

// Package v1 contains API schema definitions for the vmafx.dev/v1 API group.
// +kubebuilder:object:generate=true
// +groupName=vmafx.dev
package v1

import (
	"k8s.io/apimachinery/pkg/runtime/schema"
	"sigs.k8s.io/controller-runtime/pkg/scheme"
)

var (
	// GroupVersion is the group/version for this API.
	GroupVersion = schema.GroupVersion{Group: "vmafx.dev", Version: "v1"}

	// SchemeBuilder is used to add Go types to the GroupVersionKind scheme.
	SchemeBuilder = &scheme.Builder{GroupVersion: GroupVersion}

	// AddToScheme adds the types in this group-version to the given scheme.
	AddToScheme = SchemeBuilder.AddToScheme
)
