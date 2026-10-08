// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package v1

import (
	"reflect"
	"testing"
	"time"

	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
)

func TestResourceDeepCopiesDoNotAliasMetadata(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		copy func() (original, duplicate *metav1.ObjectMeta)
	}{
		{
			name: "job",
			copy: func() (*metav1.ObjectMeta, *metav1.ObjectMeta) {
				original := &VmafxJob{ObjectMeta: testObjectMeta()}
				duplicate := original.DeepCopy()
				return &original.ObjectMeta, &duplicate.ObjectMeta
			},
		},
		{
			name: "node",
			copy: func() (*metav1.ObjectMeta, *metav1.ObjectMeta) {
				original := &VmafxNode{ObjectMeta: testObjectMeta()}
				duplicate := original.DeepCopy()
				return &original.ObjectMeta, &duplicate.ObjectMeta
			},
		},
		{
			name: "model training",
			copy: func() (*metav1.ObjectMeta, *metav1.ObjectMeta) {
				original := &VmafxModelTraining{ObjectMeta: testObjectMeta()}
				duplicate := original.DeepCopy()
				return &original.ObjectMeta, &duplicate.ObjectMeta
			},
		},
		{
			name: "tenant",
			copy: func() (*metav1.ObjectMeta, *metav1.ObjectMeta) {
				original := &VmafxTenant{ObjectMeta: testObjectMeta()}
				duplicate := original.DeepCopy()
				return &original.ObjectMeta, &duplicate.ObjectMeta
			},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			original, duplicate := tc.copy()
			duplicate.Labels["owner"] = "copy"
			duplicate.Finalizers[0] = "copy.example"
			if original.Labels["owner"] != "original" {
				t.Error("copy aliases the original label map")
			}
			if original.Finalizers[0] != "original.example" {
				t.Error("copy aliases the original finalizer slice")
			}
		})
	}
}

func testObjectMeta() metav1.ObjectMeta {
	return metav1.ObjectMeta{
		Labels:     map[string]string{"owner": "original"},
		Finalizers: []string{"original.example"},
	}
}

// testTenant is a tenant with a value in every field kind deepCopyResource and
// the generated copies handle: type metadata, pointers to a value and to a
// struct, slices, a nested pointer in a slice element.
func testTenant() *VmafxTenant {
	enabled := true
	stamp := metav1.NewTime(time.Unix(1_700_000_000, 0).UTC())
	// A named value, not a literal in the field list: `go fix` (Go 1.27) rewrites
	// `TypeMeta: metav1.TypeMeta{Kind: ...}` of a struct that embeds TypeMeta into the
	// promoted fields, `Kind: ...`, which is not valid in a composite literal.
	typeMeta := metav1.TypeMeta{Kind: "VmafxTenant", APIVersion: "vmafx.dev/v1"}
	return &VmafxTenant{
		TypeMeta:   typeMeta,
		ObjectMeta: testObjectMeta(),
		Spec: VmafxTenantSpec{
			TenantID: "acme",
			Enabled:  &enabled,
			RBAC:     &VmafxTenantRBAC{DefaultRole: "viewer", AllowedRoles: []string{"viewer"}},
			Scoring:  &VmafxTenantScoring{Roots: []string{"/data"}},
		},
		Status: VmafxTenantStatus{
			Conditions:         []VmafxTenantCondition{{Type: "Ready", LastTransitionTime: &stamp}},
			ObservedGeneration: 3,
		},
	}
}

func TestResourceDeepCopyCopiesEveryField(t *testing.T) {
	t.Parallel()
	original := testTenant()
	duplicate := original.DeepCopy()
	if !reflect.DeepEqual(original, duplicate) {
		t.Fatalf("copy differs from the original:\n%+v\n%+v", original, duplicate)
	}
	object, ok := original.DeepCopyObject().(*VmafxTenant)
	if !ok || !reflect.DeepEqual(original, object) {
		t.Fatalf("DeepCopyObject returned %#v", object)
	}
	*duplicate.Spec.Enabled = false
	duplicate.Spec.RBAC.AllowedRoles[0] = "admin"
	duplicate.Spec.RBAC.DefaultRole = "admin"
	duplicate.Spec.Scoring.Roots[0] = "/"
	duplicate.Status.Conditions[0].LastTransitionTime.Time = time.Time{}
	if !reflect.DeepEqual(original, testTenant()) {
		t.Errorf("mutating the copy changed the original: %+v", original)
	}
}

func TestResourceDeepCopyOfNil(t *testing.T) {
	t.Parallel()
	var tenant *VmafxTenant
	if tenant.DeepCopy() != nil {
		t.Error("DeepCopy of a nil tenant is not nil")
	}
	var job *VmafxJob
	if job.DeepCopy() != nil {
		t.Error("DeepCopy of a nil job is not nil")
	}
}
