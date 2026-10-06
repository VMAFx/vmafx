// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/hardening.go — vmafx-specific defaults for the golusoris
// HTTP and gRPC servers (issue #1251, "hardened for production use").
//
// golusoris already sets slow-loris and size guards (read 30 s, header 5 s,
// write 60 s, idle 120 s, 10 MiB body, 4 MiB gRPC message). Two of its defaults
// do not fit a scoring service:
//
//   - A ScoreStream FramePair carries a raw reference and a raw distorted
//     frame. One 1080p 8-bit 4:2:0 pair is 6.2 MB, so the 4 MiB receive cap
//     rejects every stream above 720p. The server default is 64 MiB, which
//     holds a 4K 16-bit 4:2:0 pair (about 50 MB).
//   - POST /v1/score scores synchronously. A 60 s write deadline closes the
//     connection under any clip that takes longer than a minute to score. The
//     server default is 15 minutes; the request context still cancels the vmaf
//     subprocess when the client goes away.
//
// An operator value (VMAFX_GRPC_MAX_RECV_SIZE, VMAFX_HTTP_TIMEOUTS_WRITE, ...)
// always wins: the defaults apply only when the key is absent.

//go:build cgo

package main

import (
	"time"

	"github.com/golusoris/golusoris/core/config"
	grpcmod "github.com/golusoris/golusoris/grpc"
	httpserver "github.com/golusoris/golusoris/httpx/server"
	"go.uber.org/fx"
)

const (
	// defaultGRPCMaxRecvSize is the receive cap when grpc.max_recv_size is unset.
	defaultGRPCMaxRecvSize = 64 << 20
	// defaultHTTPWriteTimeout is the write deadline when http.timeouts.write is unset.
	defaultHTTPWriteTimeout = 15 * time.Minute
)

// hardeningOptions is the fx option that applies the vmafx server defaults.
func hardeningOptions() fx.Option {
	return fx.Options(
		fx.Decorate(withServerGRPCDefaults),
		fx.Decorate(withServerHTTPDefaults),
	)
}

// withServerGRPCDefaults raises the receive cap for frame streaming unless the
// operator set grpc.max_recv_size.
func withServerGRPCDefaults(framework grpcmod.Config, raw *config.Config) grpcmod.Config {
	if raw.Get("grpc.max_recv_size") == "" {
		framework.MaxRecvSize = defaultGRPCMaxRecvSize
	}
	return framework
}

// withServerHTTPDefaults lengthens the write deadline for synchronous scoring
// unless the operator set http.timeouts.write.
func withServerHTTPDefaults(framework httpserver.Options, raw *config.Config) httpserver.Options {
	if raw.Get("http.timeouts.write") == "" {
		framework.Timeouts.Write = defaultHTTPWriteTimeout
	}
	return framework
}
