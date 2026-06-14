// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

// vmafx-mcp is the Go implementation of the VMAFX Model Context Protocol
// server. It exposes the same 15 tools as the Python vmaf-mcp server over
// JSON-RPC on stdio (default) or HTTP, so that LLM clients (Claude Desktop,
// Cursor, custom MCP clients) can score video, enumerate models and backends,
// and invoke vmaf-tune workflows.
//
// Usage:
//
//	vmafx-mcp [--transport stdio|http] [--port PORT]
//
// The server exec's the vmaf CLI binary found via VMAF_BIN or the standard
// search order (see pkg/libvmaf.FindBinary). It never passes a shell string
// and refuses file paths not under the allowlisted roots.
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/modelcontextprotocol/go-sdk/mcp"

	"github.com/VMAFx/vmafx/pkg/observability"
)

const (
	serverName    = "vmafx-mcp"
	serverVersion = "1.0.0"
)

func main() {
	transport := flag.String("transport", "stdio", "Transport to use: stdio or http")
	port := flag.Int("port", 3000, "HTTP port (only used when --transport=http)")
	flag.Parse()

	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	}))

	// OpenTelemetry — non-fatal; missing OTLP collector must not block startup.
	// ADR-0782: OTel tracing wired across all Go binaries.
	otelShutdown := observability.InitOTel(context.Background(), serverName, logger)
	defer func() {
		if err := otelShutdown(context.Background()); err != nil {
			logger.Warn("otel shutdown error", "error", err)
		}
	}()

	srv := buildServer(logger)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	switch *transport {
	case "stdio":
		logger.Info("vmafx-mcp starting on stdio")
		if err := srv.Run(ctx, &mcp.StdioTransport{}); err != nil && ctx.Err() == nil {
			fmt.Fprintf(os.Stderr, "vmafx-mcp error: %v\n", err)
			os.Exit(1)
		}
	case "http":
		addr := fmt.Sprintf(":%d", *port)
		logger.Info("vmafx-mcp starting on HTTP", "addr", addr)
		handler := mcp.NewStreamableHTTPHandler(func(*http.Request) *mcp.Server { return srv }, nil)
		httpSrv := &http.Server{
			Addr:              addr,
			Handler:           handler,
			ReadHeaderTimeout: 10 * time.Second,
			ReadTimeout:       30 * time.Second,
			WriteTimeout:      120 * time.Second,
			IdleTimeout:       60 * time.Second,
		}
		ln, err := net.Listen("tcp", addr)
		if err != nil {
			fmt.Fprintf(os.Stderr, "failed to listen on %s: %v\n", addr, err)
			os.Exit(1)
		}
		go func() {
			<-ctx.Done()
			shutdownCtx, cancel := context.WithTimeout(context.Background(), observability.GracefulShutdownTimeout) //nolint:contextcheck
			defer cancel()
			_ = httpSrv.Shutdown(shutdownCtx)
		}()
		if err := httpSrv.Serve(ln); err != nil && err != http.ErrServerClosed {
			fmt.Fprintf(os.Stderr, "http server error: %v\n", err)
			os.Exit(1)
		}
	default:
		fmt.Fprintf(os.Stderr, "unknown transport %q; use stdio or http\n", *transport)
		os.Exit(2)
	}
}
