// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Command vmafx-tune-go is the Stage-1 Go port of the vmaf-tune rate-quality
// tuning CLI. It installs alongside the Python vmaf-tune binary as
// "vmafx-tune-go" to avoid naming collisions during the migration.
//
// Stage-1 ships one fully-functional subcommand:
//
//   - compare  Rate-quality sweep: compare codecs at one or more VMAF targets.
//
// All other subcommands (tune-per-shot, ladder, fast, corpus, report, …) are
// stubs that redirect users to the Python vmaf-tune binary until Stage 2.
//
// Build:
//
//	go build -ldflags "-X main.version=$(cat VERSION)" ./cmd/vmafx-tune
//
// Usage:
//
//	vmafx-tune-go compare \
//	  --reference src.mp4 \
//	  --codecs libx264,libx265 \
//	  --targets 85,90 \
//	  --output results.json
package main

import (
	"github.com/VMAFx/vmafx/cmd/vmafx-tune/cmd"
)

// version is set at build time via -ldflags.
var version = "dev"

func main() {
	cmd.Execute(version)
}
