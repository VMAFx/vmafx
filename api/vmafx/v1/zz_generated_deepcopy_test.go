// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package v1

import (
	"testing"

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
