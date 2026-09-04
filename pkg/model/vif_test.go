// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package model_test

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/VMAFx/vmafx/pkg/model"
)

// TestRequestsVIF pins the decision every libvmaf argv builder relies on:
// the v1 default needs an explicit `--feature vif`, the v0.6 generation
// does not, and a JSON model file is judged by its feature list.
func TestRequestsVIF(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	withVIF := filepath.Join(dir, "with_vif.json")
	if err := os.WriteFile(withVIF, []byte(`{"model_dict":{"feature_names":[
		"VMAF_integer_feature_adm2_score","VMAF_integer_feature_vif_scale0_score"]}}`), 0o600); err != nil {
		t.Fatal(err)
	}
	withoutVIF := filepath.Join(dir, "without_vif.json")
	if err := os.WriteFile(withoutVIF, []byte(`{"model_dict":{"feature_names":[
		"VMAF_integer_feature_adm2_score","VMAF_integer_feature_motion2_score"]}}`), 0o600); err != nil {
		t.Fatal(err)
	}
	garbage := filepath.Join(dir, "garbage.json")
	if err := os.WriteFile(garbage, []byte("not json"), 0o600); err != nil {
		t.Fatal(err)
	}

	cases := []struct {
		name  string
		model string
		want  bool
	}{
		{"empty", "", false},
		{"v1 default bare", model.DefaultVersion, false},
		{"v1 default version=", "version=" + model.DefaultVersion, false},
		{"v1 hfr", "vmaf_v1.0.16_hfr", false},
		{"v0.6.1 bare", "vmaf_v0.6.1", true},
		{"v0.6.1 version=", "version=vmaf_v0.6.1", true},
		{"v0.6.1neg", "vmaf_v0.6.1neg", true},
		{"4k v0.6.1", "vmaf_4k_v0.6.1", true},
		{"b v0.6.3", "vmaf_b_v0.6.3", true},
		{"path= with vif", "path=" + withVIF, true},
		{"path= without vif", "path=" + withoutVIF, false},
		{"path= unreadable", "path=" + filepath.Join(dir, "missing.json"), false},
		{"path= not json", "path=" + garbage, false},
		{"bare path with vif", withVIF, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := model.RequestsVIF(tc.model); got != tc.want {
				t.Errorf("RequestsVIF(%q) = %v, want %v", tc.model, got, tc.want)
			}
		})
	}
}
