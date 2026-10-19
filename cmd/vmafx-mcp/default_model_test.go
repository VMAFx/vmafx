// Copyright 2026 Lusoris. All rights reserved.
// SPDX-License-Identifier: EUPL-1.2

package main

import (
	"encoding/json"
	"strings"
	"testing"

	vmafmodel "github.com/VMAFx/vmafx/pkg/model"
)

// pinnedModelDefault is the one tool whose model default is deliberately not
// the library default: vmaf_vpl names vmaf_vpl.c's own --model default.
const pinnedModelDefault = "vmaf_vpl"

// TestScoringToolsAdvertiseTheLibraryDefaultModel fails when a scoring tool's
// input schema advertises a model default other than the library default
// (vmaf_v1.0.16_3d0h). Before this guard three schemas still said
// version=vmaf_v0.6.1 while the library scored with the v1 model.
func TestScoringToolsAdvertiseTheLibraryDefaultModel(t *testing.T) {
	want := "version=" + vmafmodel.DefaultVersion
	if defaultModelArg != want {
		t.Fatalf("defaultModelArg = %q, want %q", defaultModelArg, want)
	}
	checked := 0
	for _, tool := range servedTools(t) {
		raw, err := json.Marshal(tool.InputSchema)
		if err != nil {
			t.Fatalf("%s: marshal inputSchema: %v", tool.Name, err)
		}
		var schema struct {
			Properties map[string]struct {
				Default any `json:"default"`
			} `json:"properties"`
		}
		if err := json.Unmarshal(raw, &schema); err != nil {
			t.Fatalf("%s: parse inputSchema: %v", tool.Name, err)
		}
		prop, ok := schema.Properties["model"]
		if !ok || prop.Default == nil || tool.Name == pinnedModelDefault {
			continue
		}
		checked++
		got, _ := prop.Default.(string)
		if got != want {
			t.Errorf("tool %s advertises model default %q, want the library default %q",
				tool.Name, got, want)
		}
		if strings.Contains(got, "v0.6.1") {
			t.Errorf("tool %s still advertises a v0.6.1 default: %q", tool.Name, got)
		}
	}
	if checked < 3 {
		t.Fatalf("checked %d tool schemas with a model default, want at least 3 "+
			"(vmaf_score, vmaf_score_encoded, describe_worst_frames): the guard no longer sees them", checked)
	}
}
