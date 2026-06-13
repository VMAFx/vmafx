// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package bpf contains the eBPF FUSE-bypass program and its Go-side loader.
//
// The BPF object is compiled from rclone_bypass.bpf.c using clang + bpf2go.
// Run `go generate ./cmd/vmafx-node/bpf/` to regenerate; requires clang-16+,
// libbpf-dev, and linux-headers matching the target kernel (5.15+).
//
// ADR-0779.
package bpf

// Regenerate BPF object + Go bindings:
//
//go:generate bpf2go -cc clang -cflags "-O2 -g -Wall -Wextra -DBPF_NO_PRESERVE_ACCESS_INDEX" -target bpf rcloneBypass rclone_bypass.bpf.c -- -I/usr/include/bpf -I./
