// Copyright 2026 Lusoris. All rights reserved.
// SPDX-License-Identifier: EUPL-1.2

// tool_schema_test.go pins the outcome a permissive fallback schema would hide.
//
// A tool input schema that fails to marshal leaves the tool with no argument validation
// at all. Substituting `{"type":"object"}` -- which accepts every object -- would keep
// the tool reachable with its declared contract silently switched off, and nothing in
// the parity tests would notice, because those only compare the tools that ARE
// registered. The tests below assert the opposite outcome at each link of the chain:
// toolSchema returns the error, toolRegistrar registers nothing and stops, and
// registerTools hands the failure to buildServer, which refuses to build a server.

package main

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

// unmarshallableSchema returns a schemaObj that json.Marshal cannot encode: a channel
// has no JSON representation, so Marshal fails on the object that contains it. It stands
// in for the real regression this guards -- a schema literal that grows a value the
// encoder rejects -- without needing such a literal in tools.go.
func unmarshallableSchema() schemaObj {
	return schemaObj{
		"type":       "object",
		"properties": schemaObj{"bad": make(chan int)},
	}
}

// noopHandler is a tool handler that must never run in these tests.
func noopHandler(_ context.Context, _ map[string]any) (any, error) {
	return schemaObj{}, nil
}

// TestToolSchemaReturnsErrorNotPermissiveSchema asserts toolSchema reports a marshal
// failure instead of substituting a schema that validates nothing.
func TestToolSchemaReturnsErrorNotPermissiveSchema(t *testing.T) {
	t.Parallel()

	raw, err := toolSchema(unmarshallableSchema())
	if err == nil {
		t.Fatalf("toolSchema returned no error for an unmarshallable schema; got %s", raw)
	}
	if raw != nil {
		t.Errorf("toolSchema returned a schema alongside the error: %s", raw)
	}
	// The specific regression: the permissive empty object accepts every argument map.
	if strings.Contains(strings.ReplaceAll(string(raw), " ", ""), `{"type":"object"}`) {
		t.Errorf("toolSchema fell back to the permissive empty schema: %s", raw)
	}
}

// TestToolSchemaMarshalsAValidSchema asserts the success path is unchanged: the same
// bytes json.Marshal produces, with no error.
func TestToolSchemaMarshalsAValidSchema(t *testing.T) {
	t.Parallel()

	in := schemaObj{"type": "object", "properties": schemaObj{"ref": schemaObj{"type": "string"}}}
	raw, err := toolSchema(in)
	if err != nil {
		t.Fatalf("toolSchema: %v", err)
	}
	want, err := json.Marshal(in)
	if err != nil {
		t.Fatalf("json.Marshal: %v", err)
	}
	if string(raw) != string(want) {
		t.Errorf("toolSchema returned %s, want %s", raw, want)
	}
}

// TestToolRegistrarRefusesUnmarshallableSchema asserts the registrar does not register a
// tool whose schema failed to marshal: the error is retained, the tool's InputSchema is
// left unset rather than defaulted, and the tool never reaches the served tool list.
func TestToolRegistrarRefusesUnmarshallableSchema(t *testing.T) {
	t.Parallel()

	srv := mcp.NewServer(&mcp.Implementation{Name: serverName, Version: serverVersion}, nil)
	reg := &toolRegistrar{srv: srv}

	tool := &mcp.Tool{Name: "unmarshallable_tool", Description: "must never be registered"}
	reg.add(tool, unmarshallableSchema(), noopHandler)

	if reg.err == nil {
		t.Fatal("toolRegistrar.add accepted a tool whose schema does not marshal")
	}
	if !strings.Contains(reg.err.Error(), "unmarshallable_tool") {
		t.Errorf("retained error does not name the offending tool: %v", reg.err)
	}
	if tool.InputSchema != nil {
		t.Errorf("toolRegistrar.add wrote a substitute schema onto the tool: %s", tool.InputSchema)
	}

	for _, name := range listServedTools(t, srv) {
		if name == "unmarshallable_tool" {
			t.Fatal("a tool whose schema failed to marshal reached the served tool list")
		}
	}
}

// TestToolRegistrarStopsAfterFailure asserts a retained failure blocks every later
// registration, exercised against the real registration helpers rather than a stub, so
// no group can quietly keep registering past a failure.
func TestToolRegistrarStopsAfterFailure(t *testing.T) {
	t.Parallel()

	srv := mcp.NewServer(&mcp.Implementation{Name: serverName, Version: serverVersion}, nil)
	reg := &toolRegistrar{srv: srv}
	reg.add(&mcp.Tool{Name: "unmarshallable_tool"}, unmarshallableSchema(), noopHandler)
	firstErr := reg.err

	registerScoringTools(reg)
	registerJobControlTools(reg)

	if reg.err != firstErr {
		t.Errorf("the first failure was replaced by a later one: %v", reg.err)
	}
	if served := listServedTools(t, srv); len(served) != 0 {
		t.Errorf("registration continued after a failure; served tools: %v", served)
	}
}

// TestRegisterToolsReportsSuccessOnTheRealSurface asserts the shipped schema literals
// all marshal, so the failure path above stays a guard and not the status quo.
func TestRegisterToolsReportsSuccessOnTheRealSurface(t *testing.T) {
	t.Parallel()

	srv := mcp.NewServer(&mcp.Implementation{Name: serverName, Version: serverVersion}, nil)
	if err := registerTools(srv); err != nil {
		t.Fatalf("registerTools on the shipped tool surface: %v", err)
	}
}

// TestServedSchemasAreNeverPermissive asserts no served tool declares the bare
// `{"type":"object"}` schema -- the shape a fallback would have produced -- so every
// tool the server exposes carries the property set written in tools.go.
func TestServedSchemasAreNeverPermissive(t *testing.T) {
	t.Parallel()

	srv, err := buildServer(nil)
	if err != nil {
		t.Fatalf("buildServer: %v", err)
	}
	for _, tool := range listServedToolDefs(t, srv) {
		// InputSchema is typed any in the SDK and arrives client-side as a
		// map[string]any; marshal it back to JSON to inspect it, as the parity
		// test in server_test.go does.
		schemaBytes, err := json.Marshal(tool.InputSchema)
		if err != nil {
			t.Errorf("tool %q: failed to marshal inputSchema: %v", tool.Name, err)
			continue
		}
		var decoded map[string]any
		if err := json.Unmarshal(schemaBytes, &decoded); err != nil {
			t.Errorf("tool %q: input schema does not decode: %v", tool.Name, err)
			continue
		}
		if _, ok := decoded["properties"]; !ok {
			t.Errorf("tool %q declares a schema without properties: %s", tool.Name, schemaBytes)
		}
	}
}

// listServedToolDefs connects an in-memory client and returns the tools the server
// actually serves.
func listServedToolDefs(t *testing.T, srv *mcp.Server) []*mcp.Tool {
	t.Helper()

	client := mcp.NewClient(&mcp.Implementation{Name: "test-client", Version: "0.0.1"}, nil)
	t1, t2 := mcp.NewInMemoryTransports()
	ctx := context.Background()

	if _, err := srv.Connect(ctx, t1, nil); err != nil {
		t.Fatalf("server.Connect: %v", err)
	}
	session, err := client.Connect(ctx, t2, nil)
	if err != nil {
		t.Fatalf("client.Connect: %v", err)
	}
	defer func() {
		if err := session.Close(); err != nil {
			t.Errorf("session.Close: %v", err)
		}
	}()

	result, err := session.ListTools(ctx, &mcp.ListToolsParams{})
	if err != nil {
		t.Fatalf("ListTools: %v", err)
	}
	return result.Tools
}

// listServedTools returns just the names from listServedToolDefs.
func listServedTools(t *testing.T, srv *mcp.Server) []string {
	t.Helper()

	defs := listServedToolDefs(t, srv)
	names := make([]string, 0, len(defs))
	for _, tool := range defs {
		names = append(names, tool.Name)
	}
	return names
}
