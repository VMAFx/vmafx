// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/webhook/vmafxjob_webhook.go — VmafxJob admission webhook.
//
// Validates that VmafxJob.spec.reference and VmafxJob.spec.distorted are
// well-formed rclone URIs on CREATE and UPDATE.  A valid rclone URI must
// satisfy one of the following forms:
//   - file://...              local file path
//   - s3://...                AWS S3 or compatible
//   - rclone://<remote>/...   named rclone remote
//   - Any URI whose scheme component is a non-empty alphabetical prefix
//     followed by "://" — this covers future rclone backends
//     (gs://, azure://, sftp://, b2://, etc.).
//
// Empty strings fail validation.  The model and backend fields are
// pre-validated by the CRD OpenAPI enum constraints.
//
// ADR-0786: vmafx-operator Stage 2 — reconciler loops + webhook + per-controller RBAC.
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs (parent).

package webhook

import (
	"context"
	"fmt"
	"net/url"

	"k8s.io/apimachinery/pkg/runtime"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/webhook/admission"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
)

// VmafxJobValidator implements admission.CustomValidator for VmafxJob.
type VmafxJobValidator struct{}

var _ admission.CustomValidator = &VmafxJobValidator{}

// SetupWebhookWithManager registers the webhook with the controller-runtime manager.
func (v *VmafxJobValidator) SetupWebhookWithManager(mgr ctrl.Manager) error {
	return ctrl.NewWebhookManagedBy(mgr, &vmafxv1.VmafxJob{}).
		WithCustomValidator(v).
		Complete()
}

// ValidateCreate validates a new VmafxJob.
func (v *VmafxJobValidator) ValidateCreate(_ context.Context, obj runtime.Object) (admission.Warnings, error) {
	job, ok := obj.(*vmafxv1.VmafxJob)
	if !ok {
		return nil, fmt.Errorf("expected *VmafxJob, got %T", obj)
	}
	return nil, validateJobURIs(job)
}

// ValidateUpdate validates an updated VmafxJob.
func (v *VmafxJobValidator) ValidateUpdate(_ context.Context, _, newObj runtime.Object) (admission.Warnings, error) {
	job, ok := newObj.(*vmafxv1.VmafxJob)
	if !ok {
		return nil, fmt.Errorf("expected *VmafxJob, got %T", newObj)
	}
	return nil, validateJobURIs(job)
}

// ValidateDelete is a no-op for VmafxJob.
func (v *VmafxJobValidator) ValidateDelete(_ context.Context, _ runtime.Object) (admission.Warnings, error) {
	return nil, nil
}

// validateJobURIs checks that reference and distorted are valid rclone URIs.
func validateJobURIs(job *vmafxv1.VmafxJob) error {
	if err := validateRcloneURI("spec.reference", job.Spec.Reference); err != nil {
		return err
	}
	return validateRcloneURI("spec.distorted", job.Spec.Distorted)
}

// validateRcloneURI returns an error when uri is not a well-formed rclone URI.
// A well-formed rclone URI has a non-empty scheme (alphabetical) followed by "://".
func validateRcloneURI(field, uri string) error {
	if uri == "" {
		return fmt.Errorf("%s: must not be empty", field)
	}

	parsed, err := url.Parse(uri)
	if err != nil {
		return fmt.Errorf("%s: not a valid URI: %w", field, err)
	}

	if parsed.Scheme == "" {
		return fmt.Errorf("%s: missing URI scheme (expected file://, s3://, rclone://, etc.)", field)
	}

	// Require at least a "//" authority separator — bare scheme:path is not a
	// valid rclone URI and would confuse the rclone mount layer.
	if len(uri) < len(parsed.Scheme)+3 || uri[len(parsed.Scheme):len(parsed.Scheme)+3] != "://" {
		return fmt.Errorf("%s: URI must use scheme:// form (got %q)", field, uri)
	}

	return nil
}
