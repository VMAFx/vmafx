// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package version exposes the VMAFX module version string.
// The value is set to the contents of the repo-root VERSION file at build
// time via -ldflags; if that mechanism is not used the fallback "dev" string
// is returned so that `go test ./...` and `go vet ./...` work without any
// special build flags.
package version

// version is the build-time injected version string.
// Override with: go build -ldflags "-X github.com/VMAFx/vmafx/pkg/version.version=v3.x.y"
var version = "dev"

// Version returns the VMAFX release version string.
func Version() string {
	return version
}
