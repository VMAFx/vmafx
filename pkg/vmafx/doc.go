// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

// Package vmafx provides the generated Go binding for the VMAFx C API
// (`libvmafx.so.1`) and its status table (`core/api/vmafx.toml`).
//
// The status table (`status_gen.go`) compiles with or without cgo (`CGO_ENABLED=0`).
// With cgo enabled (`api_gen.go`), `Context`, record types, library functions,
// compile-time struct layout assertions, and structured `*Error` diagnostics are
// generated alongside the status sentinels. Callers supply `CGO_LDFLAGS` pointing
// at a VMAFx build or install directory (`-L<libdir> -lvmafx`).
package vmafx
