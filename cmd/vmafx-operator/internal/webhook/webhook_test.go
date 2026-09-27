// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/webhook/webhook_test.go — webhook validation unit tests.
//
// These are pure unit tests (no envtest / live API server needed) because the
// validators are plain Go functions with no k8s client dependency.
//
// ADR-0786: vmafx-operator Stage 2 — webhook validation + tests.
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs (parent).

package webhook_test

import (
	"context"
	"strings"
	"testing"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
	"github.com/VMAFx/vmafx/cmd/vmafx-operator/internal/webhook"
)

// ---------------------------------------------------------------------------
// VmafxJob URI validation
// ---------------------------------------------------------------------------

func TestVmafxJobValidator_Create_ValidURIs(t *testing.T) {
	v := &webhook.VmafxJobValidator{}
	ctx := context.Background()

	cases := []struct {
		name string
		ref  string
		dis  string
	}{
		{"file scheme", "file:///mnt/corpus/ref.yuv", "file:///mnt/corpus/dis.yuv"},
		{"s3 scheme", "s3://my-bucket/ref.yuv", "s3://my-bucket/dis.yuv"},
		{"rclone remote", "rclone://myremote/ref.yuv", "rclone://myremote/dis.yuv"},
		{"gs scheme", "gs://my-bucket/ref.yuv", "gs://my-bucket/dis.yuv"},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			job := &vmafxv1.VmafxJob{
				Name: "j", Namespace: "default",
				Spec: vmafxv1.VmafxJobSpec{
					Reference: tc.ref,
					Distorted: tc.dis,
				},
			}
			_, err := v.ValidateCreate(ctx, job)
			if err != nil {
				t.Errorf("expected no error for %q / %q, got: %v", tc.ref, tc.dis, err)
			}
		})
	}
}

func TestVmafxJobValidator_Create_InvalidURIs(t *testing.T) {
	v := &webhook.VmafxJobValidator{}
	ctx := context.Background()

	cases := []struct {
		name string
		ref  string
		dis  string
	}{
		{"empty reference", "", "file:///dis.yuv"},
		{"empty distorted", "file:///ref.yuv", ""},
		{"no scheme on reference", "/mnt/ref.yuv", "file:///dis.yuv"},
		{"bare colon without slashes", "s3:bucket/ref.yuv", "file:///dis.yuv"},
		{"empty both", "", ""},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			job := &vmafxv1.VmafxJob{
				Name: "j", Namespace: "default",
				Spec: vmafxv1.VmafxJobSpec{
					Reference: tc.ref,
					Distorted: tc.dis,
				},
			}
			_, err := v.ValidateCreate(ctx, job)
			if err == nil {
				t.Errorf("expected validation error for ref=%q dis=%q, got nil", tc.ref, tc.dis)
			}
		})
	}
}

func TestVmafxJobValidator_Update_Valid(t *testing.T) {
	v := &webhook.VmafxJobValidator{}
	ctx := context.Background()

	old := &vmafxv1.VmafxJob{}
	newJob := &vmafxv1.VmafxJob{
		Spec: vmafxv1.VmafxJobSpec{
			Reference: "s3://bucket/ref.yuv",
			Distorted: "s3://bucket/dis.yuv",
		},
	}
	_, err := v.ValidateUpdate(ctx, old, newJob)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
}

// An update is held to the same rules as a new object: an invalid new
// reference is rejected even when the old object was valid.
func TestVmafxJobValidator_Update_InvalidURI(t *testing.T) {
	v := &webhook.VmafxJobValidator{}
	ctx := context.Background()

	old := &vmafxv1.VmafxJob{
		Spec: vmafxv1.VmafxJobSpec{
			Reference: "s3://bucket/ref.yuv",
			Distorted: "s3://bucket/dis.yuv",
		},
	}
	newJob := &vmafxv1.VmafxJob{
		Spec: vmafxv1.VmafxJobSpec{
			Reference: "/mnt/ref.yuv",
			Distorted: "s3://bucket/dis.yuv",
		},
	}
	if _, err := v.ValidateUpdate(ctx, old, newJob); err == nil {
		t.Fatal("expected validation error for an update without a URI scheme, got nil")
	}
}

func TestVmafxJobValidator_Update_WrongType(t *testing.T) {
	v := &webhook.VmafxJobValidator{}
	ctx := context.Background()

	_, err := v.ValidateUpdate(ctx, &vmafxv1.VmafxJob{}, &vmafxv1.VmafxNode{})
	if err == nil || !strings.Contains(err.Error(), "expected *VmafxJob") {
		t.Fatalf("expected a type error naming *VmafxJob, got %v", err)
	}
}

func TestVmafxJobValidator_Delete_NoOp(t *testing.T) {
	v := &webhook.VmafxJobValidator{}
	ctx := context.Background()
	_, err := v.ValidateDelete(ctx, &vmafxv1.VmafxJob{})
	if err != nil {
		t.Fatalf("unexpected error on delete: %v", err)
	}
}

// ---------------------------------------------------------------------------
// VmafxNode GPU vendor validation
// ---------------------------------------------------------------------------

func TestVmafxNodeValidator_Create_ValidVendors(t *testing.T) {
	v := &webhook.VmafxNodeValidator{}
	ctx := context.Background()

	for _, vendor := range []string{"nvidia", "amd", "intel", "cpu"} {
		t.Run(vendor, func(t *testing.T) {
			node := &vmafxv1.VmafxNode{
				Name: "n", Namespace: "default",
				Spec: vmafxv1.VmafxNodeSpec{GPUVendor: vendor},
			}
			_, err := v.ValidateCreate(ctx, node)
			if err != nil {
				t.Errorf("vendor %q: unexpected error: %v", vendor, err)
			}
		})
	}
}

func TestVmafxNodeValidator_Create_InvalidVendor(t *testing.T) {
	v := &webhook.VmafxNodeValidator{}
	ctx := context.Background()

	for _, vendor := range []string{"", "NVIDIA", "arm", "fpga", "x86"} {
		t.Run(vendor, func(t *testing.T) {
			node := &vmafxv1.VmafxNode{
				Name: "n", Namespace: "default",
				Spec: vmafxv1.VmafxNodeSpec{GPUVendor: vendor},
			}
			_, err := v.ValidateCreate(ctx, node)
			if err == nil {
				t.Errorf("vendor %q: expected error, got nil", vendor)
			}
		})
	}
}

func TestVmafxNodeValidator_Update_Valid(t *testing.T) {
	v := &webhook.VmafxNodeValidator{}
	ctx := context.Background()

	old := &vmafxv1.VmafxNode{}
	newNode := &vmafxv1.VmafxNode{
		Spec: vmafxv1.VmafxNodeSpec{GPUVendor: "intel"},
	}
	_, err := v.ValidateUpdate(ctx, old, newNode)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
}

// An update is held to the same rules as a new object.
func TestVmafxNodeValidator_Update_InvalidVendor(t *testing.T) {
	v := &webhook.VmafxNodeValidator{}
	ctx := context.Background()

	old := &vmafxv1.VmafxNode{Spec: vmafxv1.VmafxNodeSpec{GPUVendor: "intel"}}
	newNode := &vmafxv1.VmafxNode{Spec: vmafxv1.VmafxNodeSpec{GPUVendor: "fpga"}}
	if _, err := v.ValidateUpdate(ctx, old, newNode); err == nil {
		t.Fatal("expected validation error for an unknown GPU vendor on update, got nil")
	}
}

func TestVmafxNodeValidator_Update_WrongType(t *testing.T) {
	v := &webhook.VmafxNodeValidator{}
	ctx := context.Background()

	_, err := v.ValidateUpdate(ctx, &vmafxv1.VmafxNode{}, &vmafxv1.VmafxJob{})
	if err == nil || !strings.Contains(err.Error(), "expected *VmafxNode") {
		t.Fatalf("expected a type error naming *VmafxNode, got %v", err)
	}
}
