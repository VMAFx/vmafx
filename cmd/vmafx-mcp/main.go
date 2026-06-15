// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

// vmafx-mcp is the Go implementation of the VMAFX Model Context Protocol
// server. It exposes the same tools as the Python vmaf-mcp server over
// JSON-RPC on stdio (default) or streamable HTTP, so that LLM clients (Claude
// Desktop, Cursor, custom MCP clients) can score video, enumerate models and
// backends, and invoke vmaf-tune workflows.
//
// Composition root: the server is wired as an fx application over the golusoris
// framework (ADR-1119 Phase-1 PR-5). golusoris owns config (koanf), structured
// slog logging, OpenTelemetry, the clock, and signal handling / graceful
// shutdown. The MCP server itself is NOT a golusoris server module (golusoris
// has no MCP module), so its transport is wired directly in an fx lifecycle
// hook (runMCPTransport): OnStart launches the stdio OR streamable-HTTP
// transport per config, OnStop shuts it down. This file supplies only the MCP
// domain provider (buildMCPServer, which reuses the existing buildServer tool
// surface) and the transport invoke.
//
// stdio-stdout purity (R3, ADR-1119): the stdio transport owns stdin/stdout for
// the JSON-RPC framing — a single stray write to stdout corrupts the stream.
// The golusoris log module writes to STDERR, otel.Module is OTLP-gRPC (no
// stdout writes; a silent no-op when no exporter is configured), and
// bootstrap.FxLogger routes fx's lifecycle events through slog → stderr. The
// only legitimate stdout writer in stdio mode is the MCP transport. Nothing in
// the fx graph writes to stdout. Do not add an fx.Print logger or a stdout OTel
// exporter without gating them off in stdio mode.
//
// Configuration (koanf via golusoris/config, env prefix VMAFX_, "." delimiter).
// The golusoris env transform strips the VMAFX_ prefix, lowercases, and
// replaces EVERY underscore with the "." delimiter, so the koanf key is the
// dotted form shown in the third column:
//
//	VMAFX_MCP_TRANSPORT   -> mcp.transport    Transport: "stdio" (default) or "http".
//	VMAFX_MCP_HTTP_ADDR   -> mcp.http.addr    HTTP listen address (default ":3000").
//	VMAFX_LOG_LEVEL       (-> LOG_LEVEL, bridged below — golusoris#234) slog level.
//	VMAFX_LOG_FORMAT      (-> LOG_FORMAT, bridged below — golusoris#234) log handler.
//	VMAF_BIN              Path to the vmaf CLI binary (read directly by the tool
//	                      handlers, NOT via koanf — unchanged contract).
//	VMAFX_MCP_DIRECT      Set to "1" to enable the direct cgo scoring path
//	                      (read directly in impl_direct.go — unchanged contract).
//
// NOTE on the env-var / flag contract change (ADR-1119): the pre-fx server took
// two CLI flags, --transport (stdio|http) and --port (int, default 3000). The
// fx root is config-driven and drops the flags; the equivalents are the env
// vars VMAFX_MCP_TRANSPORT and VMAFX_MCP_HTTP_ADDR. The historical default port
// 3000 is preserved as the default address ":3000". Operators using --transport
// http --port N must migrate to VMAFX_MCP_TRANSPORT=http VMAFX_MCP_HTTP_ADDR=:N.
//
// The server exec's the vmaf CLI binary found via VMAF_BIN or the standard
// search order (see pkg/libvmaf.FindBinary). It never passes a shell string and
// refuses file paths not under the allowlisted roots.
//
// ADR-0782: OpenTelemetry tracing across all Go binaries (original root).
// ADR-1119: golusoris fx framework adoption.
package main

import (
	"context"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"time"

	"github.com/modelcontextprotocol/go-sdk/mcp"
	"go.uber.org/fx"

	"github.com/golusoris/golusoris/config"

	"github.com/VMAFx/vmafx/internal/app/bootstrap"
)

const (
	serverName    = "vmafx-mcp"
	serverVersion = "1.0.0"

	// defaultMCPTransport is used when VMAFX_MCP_TRANSPORT is unset. stdio is
	// the historical default and the one IDE/MCP clients expect.
	defaultMCPTransport = "stdio"

	// defaultMCPHTTPAddr preserves the pre-fx default --port 3000 as a full
	// listen address. Used when VMAFX_MCP_HTTP_ADDR is unset.
	defaultMCPHTTPAddr = ":3000"

	// gracefulShutdownTimeout bounds the HTTP transport's graceful drain on
	// OnStop. Mirrors the pre-fx observability.GracefulShutdownTimeout.
	gracefulShutdownTimeout = 30 * time.Second
)

func main() {
	// Interim env bridge, applied BEFORE fx.New so the log module observes it.
	// golusoris#234: the pinned v0.4.0 log module reads bare LOG_LEVEL/LOG_FORMAT
	// and ignores the VMAFX_ prefix (the prefixed read is merged to golusoris
	// main but untagged). Bridge the prefixed vars across so operators configure
	// the level through the same prefix as everything else; remove once the
	// carrying golusoris tag lands. Mirrors cmd/vmafx-server/main.go.
	if v := os.Getenv("VMAFX_LOG_LEVEL"); v != "" && os.Getenv("LOG_LEVEL") == "" {
		_ = os.Setenv("LOG_LEVEL", v)
	}
	if v := os.Getenv("VMAFX_LOG_FORMAT"); v != "" && os.Getenv("LOG_FORMAT") == "" {
		_ = os.Setenv("LOG_FORMAT", v)
	}

	fx.New(
		// golusoris foundation: config + log + clock + id + validate + crypto,
		// the OTel module, and the build-version supply (ADR-1119). bootstrap.Base
		// deliberately carries NO server module — the MCP transport is not a
		// golusoris server; it is wired in the runMCPTransport lifecycle hook.
		bootstrap.Base,
		// Override the env prefix so the whole graph reads VMAFX_* config keys.
		// Watch is enabled so a mounted k8s ConfigMap update triggers reload.
		fx.Replace(config.Options{EnvPrefix: "VMAFX_", Delimiter: ".", Watch: true}),
		// Route fx lifecycle events onto the golusoris slog logger (stderr). This
		// is load-bearing for stdio-stdout purity (R3): fx's default printer
		// writes to stderr, but routing through slog keeps it on the same stream
		// and under OTel correlation, and guarantees it never touches stdout.
		bootstrap.FxLogger(),

		// Domain provider: the MCP server with the full tool surface. Reuses the
		// existing buildServer (tools.go / impl.go), unchanged, so the Go↔Python
		// byte-compat schema/argv surface is untouched.
		fx.Provide(buildMCPServer),

		// Wire the MCP transport (stdio OR streamable-HTTP per config) in an fx
		// lifecycle hook. OnStart launches it, OnStop shuts it down.
		fx.Invoke(runMCPTransport),
	).Run()
}

// buildMCPServer is the fx provider for the MCP server. It reuses the existing
// buildServer (which registers the full tool surface from tools.go) so the
// Go↔Python byte-identical scoring schema/argv contract (cmd/vmafx-mcp/AGENTS.md
// invariant #10) is untouched by the framework migration. The *config.Config
// dependency is taken for forward-looking config-driven tool wiring and so the
// provider participates in the graph in the same shape as the sibling
// migrations; buildServer itself currently needs only the logger.
func buildMCPServer(log *slog.Logger, _ *config.Config) *mcp.Server {
	return buildServer(log)
}

// runMCPTransport wires the MCP transport into the fx lifecycle. golusoris has
// no MCP server module, so the transport is owned here directly.
//
// OnStart selects the transport from config and launches it on a background
// goroutine (OnStart must not block — it has to return so fx finishes startup):
//
//   - stdio: srv.Run(ctx, &mcp.StdioTransport{}) reads JSON-RPC framing on
//     stdin and writes responses on stdout. It blocks until the client closes
//     stdin or the run context is cancelled. When Run returns on its own (client
//     disconnect), we ask fx to shut the whole app down so the process exits.
//     R3: the StdioTransport owns stdout; nothing else in the graph writes there.
//
//   - http: an *http.Server serves the streamable-HTTP handler on
//     mcp.http.addr. OnStop drains it gracefully.
//
// runErr carries a transport failure out of the goroutine; OnStop reports it.
func runMCPTransport(
	lc fx.Lifecycle,
	srv *mcp.Server,
	cfg *config.Config,
	log *slog.Logger,
	sd fx.Shutdowner,
) error {
	transport := cfg.Get("mcp.transport")
	if transport == "" {
		transport = defaultMCPTransport
	}

	switch transport {
	case "stdio":
		runCtx, cancel := context.WithCancel(context.Background())
		lc.Append(fx.Hook{
			OnStart: func(_ context.Context) error {
				log.Info("vmafx-mcp starting on stdio")
				go func() {
					// srv.Run blocks until stdin closes or runCtx is cancelled.
					err := srv.Run(runCtx, &mcp.StdioTransport{})
					if err != nil && runCtx.Err() == nil {
						// Genuine transport failure (not our own cancel): log to
						// stderr and ask fx to exit non-zero.
						log.Error("vmafx-mcp stdio transport error", "error", err)
						_ = sd.Shutdown(fx.ExitCode(1))
						return
					}
					// Clean end (client closed stdin) or our cancel: shut the app
					// down so the process exits instead of hanging in Run().
					_ = sd.Shutdown()
				}()
				return nil
			},
			OnStop: func(_ context.Context) error {
				cancel()
				return nil
			},
		})
		return nil

	case "http":
		addr := cfg.Get("mcp.http.addr")
		if addr == "" {
			addr = defaultMCPHTTPAddr
		}
		handler := mcp.NewStreamableHTTPHandler(
			func(*http.Request) *mcp.Server { return srv }, nil)
		httpSrv := &http.Server{
			Addr:              addr,
			Handler:           handler,
			ReadHeaderTimeout: 10 * time.Second,
			ReadTimeout:       30 * time.Second,
			WriteTimeout:      120 * time.Second,
			IdleTimeout:       60 * time.Second,
		}
		lc.Append(fx.Hook{
			OnStart: func(_ context.Context) error {
				ln, err := net.Listen("tcp", addr)
				if err != nil {
					return fmt.Errorf("listen on %s: %w", addr, err)
				}
				log.Info("vmafx-mcp starting on HTTP", "addr", addr)
				go func() {
					if serveErr := httpSrv.Serve(ln); serveErr != nil &&
						serveErr != http.ErrServerClosed {
						log.Error("vmafx-mcp http transport error", "error", serveErr)
						_ = sd.Shutdown(fx.ExitCode(1))
					}
				}()
				return nil
			},
			OnStop: func(ctx context.Context) error {
				shutdownCtx, cancel := context.WithTimeout(ctx, gracefulShutdownTimeout)
				defer cancel()
				return httpSrv.Shutdown(shutdownCtx)
			},
		})
		return nil

	default:
		return fmt.Errorf("unknown transport %q; set VMAFX_MCP_TRANSPORT to stdio or http", transport)
	}
}
