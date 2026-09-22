// Copyright 2026 Lusoris. All rights reserved.
// SPDX-License-Identifier: EUPL-1.2

package main

import (
	"fmt"
	"log/slog"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

// buildServer constructs the MCP server and registers all 15 VMAFX tools.
// The tool definitions match the Python vmaf-mcp server exactly so that
// IDE clients (Claude Desktop, Cursor) work unchanged.
//
// A tool whose input schema fails to marshal is a source defect in tools.go, and it is
// fatal here: the half-built server is discarded and the error returned, so the process
// never serves a tool surface that is missing a tool or validating one of them with a
// weaker schema than the one written in tools.go.
func buildServer(logger *slog.Logger) (*mcp.Server, error) {
	srv := mcp.NewServer(&mcp.Implementation{
		Name:    serverName,
		Version: serverVersion,
	}, &mcp.ServerOptions{
		Logger: logger,
	})

	if err := registerTools(srv); err != nil {
		return nil, fmt.Errorf("registering the MCP tool surface: %w", err)
	}
	return srv, nil
}
