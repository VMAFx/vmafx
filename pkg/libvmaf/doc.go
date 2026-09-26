// Copyright 2026 Lusoris. All rights reserved.
// SPDX-License-Identifier: EUPL-1.2

// Package libvmaf provides a Go interface to the libvmaf C shared library.
//
// It exposes a thin wrapper around the libvmaf public C API sufficient to
// invoke VMAF scoring from Go code. The package is used by the vmafx-mcp
// and vmafx-server binaries.
//
// # Build requirements
//
// The host must have this fork's libvmaf headers and shared library. The
// recommended local workflow uses the canonical cgo build directory:
//
//	meson setup core/build-cpu core && ninja -C core/build-cpu
//	make go-build
//
// Direct Go commands must select the library explicitly:
//
//	export CGO_LDFLAGS="-L$(pwd)/core/build-cpu/src -lvmaf -lm"
//	export LD_LIBRARY_PATH="$(pwd)/core/build-cpu/src${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
//	go test ./pkg/libvmaf
//
// There is intentionally no implicit `-lvmaf` fallback in the package. This
// prevents a missing fork build from silently linking a distro or stale
// system library. Container builds may point CGO_LDFLAGS at their separately
// verified, staged fork library instead.
//
// # Scope
//
// This package intentionally exposes only what vmafx-mcp requires:
// path-validation helpers and subprocess-based scoring delegation. The full
// libvmaf C API is deliberately NOT wrapped here — the Go MCP server
// delegates scoring to the vmaf CLI binary (same approach as the Python
// server) and does not link against libvmaf.so at runtime. A future PR
// may add direct cgo scoring for embedded use cases.
package libvmaf
