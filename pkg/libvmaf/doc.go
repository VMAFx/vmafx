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
//	export CGO_LDFLAGS="-L$(pwd)/core/build-cpu/src -lvmaf -lvmafx -lm"
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
// The package reaches libvmaf four ways; three of them are cgo calls into
// libvmaf.so, so every binary importing the package links the library and
// needs it at run time:
//
//   - [Scorer] runs the vmaf CLI binary as a subprocess and parses its JSON
//     (the vmafx-mcp default and the vmafx-server unary Score path).
//   - [ScoreDirect] scores a file pair in process through cgo (ADR-0931;
//     vmafx-mcp with VMAFX_MCP_DIRECT=1).
//   - [StreamScorer] keeps a cgo VmafContext and scores raw frames pushed one
//     at a time (ADR-0933; the gRPC ScoreStream path of vmafx-server).
//   - [DNNSession] opens a tiny-AI ONNX session through cgo (`dnn.h`).
//
// It also holds the path-validation helpers the tool handlers use. Only what
// these callers need is wrapped; the full C API is not.
package libvmaf
