// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package version exposes the VMAFX module version string.
//
// There is no repo-root VERSION file. The release-owned product version lives
// in .release-please-manifest.json and the coordinated x-release-please-version
// markers (ADR-1151); the publish workflows pass the published tag down as the
// VMAFX_VERSION build argument, and the Go image stages turn it into
// -ldflags "-X github.com/VMAFx/vmafx/pkg/version.version=${VMAFX_VERSION}".
// Without that override the fallback "dev" string is returned, so
// `go test ./...` and `go vet ./...` work with no special build flags.
package version

import "runtime/debug"

// version is the build-time injected version string.
// Override with: go build -ldflags "-X github.com/VMAFx/vmafx/pkg/version.version=v1.x.y"
var version = "dev"

// Version returns the VMAFX release version string.
func Version() string {
	return version
}

// Info is the typed build-version record supplied into the fx graph by
// internal/app/bootstrap (ADR-1119). golusoris shipped its own version module
// in v0.5.0 (golusoris.Version, golusoris#226); vmafx keeps this local Info
// deliberately because it also carries the VCS revision + build metadata read
// from runtime/debug (below), which the framework's ldflags-only string omits.
type Info struct {
	// Version is the ldflags-injected release string (else "dev").
	Version string
	// Revision is the VCS commit hash from the build's debug info, if any.
	Revision string
	// Time is the VCS commit time (RFC 3339), if recorded.
	Time string
	// Dirty reports whether the working tree was modified at build time.
	Dirty bool
	// Go is the Go toolchain version that produced the binary.
	Go string
}

// Get assembles the build [Info] from the ldflags-injected [Version] plus the
// VCS settings embedded by the Go toolchain (runtime/debug.ReadBuildInfo).
// Missing VCS settings yield empty fields rather than an error so the call is
// always safe in tests and non-VCS builds.
func Get() Info {
	info := Info{Version: version, Go: "unknown"}
	bi, ok := debug.ReadBuildInfo()
	if !ok {
		return info
	}
	info.Go = bi.GoVersion
	for _, s := range bi.Settings {
		switch s.Key {
		case "vcs.revision":
			info.Revision = s.Value
		case "vcs.time":
			info.Time = s.Value
		case "vcs.modified":
			info.Dirty = s.Value == "true"
		default:
			// other build settings are not part of the version record
		}
	}
	return info
}
