// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/webhook/vmafxnode_webhook.go — VmafxNode admission webhook.
//
// Validates that VmafxNode.spec.gpuVendor is one of the accepted values on
// CREATE and UPDATE.  Accepted values: nvidia, amd, intel, cpu.
//
// The CRD OpenAPI schema already enforces this constraint as an enum; the
// webhook provides defense-in-depth at admission time and produces a clearer
// error message than the generated schema validation.
//
// ADR-0786: vmafx-operator Stage 2 — reconciler loops + webhook + per-controller RBAC.
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs (parent).

package webhook

import (
	"context"
	"fmt"

	"k8s.io/apimachinery/pkg/runtime"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/webhook/admission"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
)

// acceptedGPUVendors is the exhaustive list of allowed gpuVendor values.
var acceptedGPUVendors = map[string]struct{}{
	"nvidia": {},
	"amd":    {},
	"intel":  {},
	"cpu":    {},
}

// VmafxNodeValidator implements admission.CustomValidator for VmafxNode.
type VmafxNodeValidator struct{}

var _ admission.CustomValidator = &VmafxNodeValidator{}

// SetupWebhookWithManager registers the webhook with the controller-runtime manager.
func (v *VmafxNodeValidator) SetupWebhookWithManager(mgr ctrl.Manager) error {
	return ctrl.NewWebhookManagedBy(mgr, &vmafxv1.VmafxNode{}).
		WithCustomValidator(v).
		Complete()
}

// ValidateCreate validates a new VmafxNode.
func (v *VmafxNodeValidator) ValidateCreate(_ context.Context, obj runtime.Object) (admission.Warnings, error) {
	node, ok := obj.(*vmafxv1.VmafxNode)
	if !ok {
		return nil, fmt.Errorf("expected *VmafxNode, got %T", obj)
	}
	return nil, validateGPUVendor(node)
}

// ValidateUpdate validates an updated VmafxNode.
func (v *VmafxNodeValidator) ValidateUpdate(_ context.Context, _, newObj runtime.Object) (admission.Warnings, error) {
	node, ok := newObj.(*vmafxv1.VmafxNode)
	if !ok {
		return nil, fmt.Errorf("expected *VmafxNode, got %T", newObj)
	}
	return nil, validateGPUVendor(node)
}

// ValidateDelete is a no-op for VmafxNode.
func (v *VmafxNodeValidator) ValidateDelete(_ context.Context, _ runtime.Object) (admission.Warnings, error) {
	return nil, nil
}

// validateGPUVendor checks that spec.gpuVendor is one of the accepted values.
func validateGPUVendor(node *vmafxv1.VmafxNode) error {
	if _, ok := acceptedGPUVendors[node.Spec.GPUVendor]; !ok {
		return fmt.Errorf(
			"spec.gpuVendor: %q is not valid; must be one of nvidia, amd, intel, cpu",
			node.Spec.GPUVendor,
		)
	}
	return nil
}
