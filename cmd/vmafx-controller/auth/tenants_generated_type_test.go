// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package auth_test

import (
	"reflect"
	"testing"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
)

// The registry decodes the VmafxTenant spec type generated from
// api/vmafx-platform.toml (ADR-2350 D13), never a hand-written copy of it: a
// copy would drift from the CRD the chart installs.
func TestTenantSpecIsTheGeneratedType(t *testing.T) {
	t.Parallel()
	pairs := []struct {
		name      string
		auth, gen reflect.Type
	}{
		{"spec", reflect.TypeFor[auth.TenantSpec](), reflect.TypeFor[vmafxv1.VmafxTenantSpec]()},
		{"oidc", reflect.TypeFor[auth.TenantOIDC](), reflect.TypeFor[vmafxv1.VmafxTenantOIDC]()},
		{"rbac", reflect.TypeFor[auth.TenantRBAC](), reflect.TypeFor[vmafxv1.VmafxTenantRBAC]()},
		{"scoring", reflect.TypeFor[auth.TenantScoring](), reflect.TypeFor[vmafxv1.VmafxTenantScoring]()},
	}
	for _, pair := range pairs {
		if pair.auth != pair.gen {
			t.Errorf("auth tenant %s type is %v, not the generated %v", pair.name, pair.auth, pair.gen)
		}
	}
	spec := vmafxv1.VmafxTenant{Spec: auth.TenantSpec{TenantID: "acme"}}
	if spec.Spec.TenantID != "acme" {
		t.Error("the generated resource does not carry the auth spec")
	}
}
