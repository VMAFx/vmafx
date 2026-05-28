// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

package main

import (
	"log/slog"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

// buildServer constructs the MCP server and registers all 16 VMAFX tools.
// The tool definitions match the Python vmaf-mcp server exactly so that
// IDE clients (Claude Desktop, Cursor) work unchanged.
func buildServer(logger *slog.Logger) *mcp.Server {
	srv := mcp.NewServer(&mcp.Implementation{
		Name:    serverName,
		Version: serverVersion,
	}, &mcp.ServerOptions{
		Logger: logger,
	})

	registerTools(srv)
	return srv
}
