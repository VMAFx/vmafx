// Copyright 2026 Lusoris. All rights reserved.
// SPDX-License-Identifier: EUPL-1.2

package main

import (
	"log/slog"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

// buildServer constructs the MCP server and registers all 15 VMAFX tools.
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
