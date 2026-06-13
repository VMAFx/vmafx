// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/ladder_nan_test.go — regression coverage for the
// ladder emitter's NaN/Inf handling. The prior emitLadderJSON only
// sanitised Point.BitratekBps in Cloud — not Point.VMAF, not the Hull
// slice, not the Renditions slice. A NaN that reached any of those
// would crash json.MarshalIndent and abort the whole ladder report,
// breaking the Python ↔ Go parser-parity invariant (AGENTS.md #2).

package cmd

import (
	"encoding/json"
	"math"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/ladder"
)

func TestEmitLadderJSON_NaNInHullAndRenditions(t *testing.T) {
	t.Parallel()
	flags := &ladderFlags{
		reference:   "src.mp4",
		resolutions: []string{"640x480"},
		targets:     []float64{85.0},
	}
	// Construct a result where every slice (Cloud, Hull, Renditions)
	// contains at least one non-finite value across every float field.
	res := ladder.LadderResult{
		Src:     "src.mp4",
		Encoder: "libx264",
		Cloud: []ladder.Point{
			{Width: 640, Height: 480, BitratekBps: math.NaN(), VMAF: 90.0, CRF: 28, TargetVMAF: 85.0, OK: false},
			{Width: 640, Height: 480, BitratekBps: 800.0, VMAF: math.Inf(1), CRF: 28, TargetVMAF: math.NaN(), OK: false},
		},
		Hull: []ladder.Point{
			{Width: 640, Height: 480, BitratekBps: math.NaN(), VMAF: math.NaN(), CRF: 28, TargetVMAF: 85.0, OK: true},
		},
		Renditions: []ladder.Rendition{
			{Width: 640, Height: 480, BitratekBps: math.Inf(-1), VMAF: math.NaN(), CRF: 28},
		},
	}

	out, err := emitLadderJSON(res, flags, 100.0)
	if err != nil {
		t.Fatalf("emitLadderJSON returned error on non-finite payload: %v", err)
	}
	if strings.Contains(out, "NaN") || strings.Contains(out, "Infinity") {
		t.Errorf("emitLadderJSON output still contains bare NaN/Infinity tokens:\n%s", out)
	}
	var got ladderWirePayload
	if err := json.Unmarshal([]byte(out), &got); err != nil {
		t.Fatalf("emitLadderJSON output is not valid JSON: %v\n%s", err, out)
	}
	// Every float field must be finite after sanitisation.
	for i, p := range got.Cloud {
		if math.IsNaN(p.BitratekBps) || math.IsInf(p.BitratekBps, 0) ||
			math.IsNaN(p.VMAF) || math.IsInf(p.VMAF, 0) ||
			math.IsNaN(p.TargetVMAF) || math.IsInf(p.TargetVMAF, 0) {
			t.Errorf("Cloud[%d] still has non-finite floats: %+v", i, p)
		}
	}
	for i, p := range got.Hull {
		if math.IsNaN(p.BitratekBps) || math.IsInf(p.BitratekBps, 0) ||
			math.IsNaN(p.VMAF) || math.IsInf(p.VMAF, 0) {
			t.Errorf("Hull[%d] still has non-finite floats: %+v", i, p)
		}
	}
	for i, r := range got.Renditions {
		if math.IsNaN(r.BitratekBps) || math.IsInf(r.BitratekBps, 0) ||
			math.IsNaN(r.VMAF) || math.IsInf(r.VMAF, 0) {
			t.Errorf("Renditions[%d] still has non-finite floats: %+v", i, r)
		}
	}
}
