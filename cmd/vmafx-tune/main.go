// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Command vmafx-tune-go is the Go port of the vmaf-tune rate-quality
// tuning CLI.
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
