// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/unit_internal_fixtures_test.go — fixture builders for
// unit_internal_test.go. Kept in a separate file so the production-code
// imports stay tidy and the data-only helpers are easy to scan.

package cmd

import (
	"io"
	"log/slog"
	"math"

	"github.com/VMAFx/vmafx/pkg/ladder"
)

// testDeps returns a deps value suitable for unit tests that call the run*
// functions directly without standing up a golusoris fx graph. The logger
// discards output; Cfg is left nil because the run* functions log and act on
// flags, not on config (config is exercised through the live fx path).
func testDeps() deps {
	return deps{
		Log: slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
}

// makeLadderResult returns a minimal LadderResult with one cloud point, one
// hull point, and one rendition — enough to exercise both the JSON and
// Markdown rendering paths.
func makeLadderResult() ladder.LadderResult {
	return ladder.LadderResult{
		Src:     "src.mp4",
		Encoder: "libx264",
		Cloud: []ladder.Point{
			{Width: 640, Height: 480, BitratekBps: 800.0, VMAF: 85.5, CRF: 28, TargetVMAF: 85.0, OK: true},
			{Width: 1920, Height: 1080, BitratekBps: 2500.0, VMAF: 95.2, CRF: 22, TargetVMAF: 95.0, OK: true},
		},
		Hull: []ladder.Point{
			{Width: 640, Height: 480, BitratekBps: 800.0, VMAF: 85.5, CRF: 28, TargetVMAF: 85.0, OK: true},
			{Width: 1920, Height: 1080, BitratekBps: 2500.0, VMAF: 95.2, CRF: 22, TargetVMAF: 95.0, OK: true},
		},
		Renditions: []ladder.Rendition{
			{Width: 640, Height: 480, BitratekBps: 800.0, VMAF: 85.5, CRF: 28},
			{Width: 1920, Height: 1080, BitratekBps: 2500.0, VMAF: 95.2, CRF: 22},
		},
	}
}

// makeLadderResultWithNaN returns a LadderResult whose cloud contains a NaN
// bitrate, used to exercise emitLadderJSON's sanitisation branch.
func makeLadderResultWithNaN() ladder.LadderResult {
	return ladder.LadderResult{
		Src:     "src.mp4",
		Encoder: "libx264",
		Cloud: []ladder.Point{
			{Width: 640, Height: 480, BitratekBps: math.NaN(), VMAF: 85.5, CRF: 28, TargetVMAF: 85.0, OK: false, Error: "fail"},
			{Width: 640, Height: 480, BitratekBps: math.Inf(1), VMAF: 0, CRF: 0, TargetVMAF: 85.0, OK: false, Error: "fail"},
		},
		Hull:       []ladder.Point{},
		Renditions: []ladder.Rendition{},
	}
}

// makeEmptyLadderResult returns a result with no cloud, no hull, no
// renditions — exercises emitLadderMarkdown's empty-state branches.
func makeEmptyLadderResult() ladder.LadderResult {
	return ladder.LadderResult{
		Src:        "x.mp4",
		Encoder:    "libx264",
		Cloud:      []ladder.Point{},
		Hull:       []ladder.Point{},
		Renditions: []ladder.Rendition{},
	}
}
