// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package main

import "testing"

func TestVersionRequest(t *testing.T) {
	t.Parallel()
	if !isVersionRequest([]string{"vmafx-mcp", "--version"}) {
		t.Fatal("--version must select the non-blocking version path")
	}
	if isVersionRequest([]string{"vmafx-mcp"}) {
		t.Fatal("normal startup must not select the version path")
	}
	if isVersionRequest([]string{"vmafx-mcp", "--version", "extra"}) {
		t.Fatal("version path must reject extra arguments")
	}
	if isVersionRequest([]string{"vmafx-mcp", "-version"}) {
		t.Fatal("only the exact --version flag selects the version path")
	}
}
