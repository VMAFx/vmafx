// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

// Package libvmaf provides a Go interface to the libvmaf C shared library.
//
// It exposes a thin wrapper around the libvmaf public C API sufficient to
// invoke VMAF scoring from Go code. The package is used by the vmafx-mcp
// and vmafx-server binaries.
//
// # Build requirements
//
// The host must have libvmaf installed (headers + shared library). The
// recommended approach is to build the fork with:
//
//	meson setup build && ninja -C build && ninja -C build install
//
// Or to point to an in-tree build via CGO_CFLAGS / CGO_LDFLAGS:
//
//	export CGO_CFLAGS="-I$(pwd)/core/include"
//	export CGO_LDFLAGS="-L$(pwd)/core/build -lvmaf -Wl,-rpath,$(pwd)/core/build"
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
