// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/fixed_port_guard_test.go — no Go test may bind a fixed TCP
// port. A fixed port makes parallel test runs and CI retries flake with
// "address already in use" (the TestRunHTTPGracefulShutdown failure of issue
// #1251). Tests bind "127.0.0.1:0" and read the address back.

//go:build cgo

package main

import (
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// fixedPortBindRe matches a bind to a literal non-zero port.
var fixedPortBindRe = regexp.MustCompile(
	`(?:Listen\(\s*"tcp[46]?"\s*,\s*"[^"]*:[1-9][0-9]*"|ListenAndServe(?:TLS)?\(\s*"[^"]*:[1-9][0-9]*")`)

func TestFixedPortBindMatcher(t *testing.T) {
	t.Parallel()
	for src, want := range map[string]bool{
		`net.Listen("tcp", ":8080")`:           true,
		`net.Listen("tcp", "127.0.0.1:19090")`: true,
		`http.ListenAndServe(":9090", nil)`:    true,
		`net.Listen("tcp", "127.0.0.1:0")`:     false,
		`net.Listen("unix", "/tmp/x.sock")`:    false,
		`t.Setenv("VMAFX_HTTP_ADDR", ":8080")`: false,
		`net.Listen("tcp", ":0")`:              false,
	} {
		if got := fixedPortBindRe.MatchString(src); got != want {
			t.Errorf("matcher(%q) = %v, want %v", src, got, want)
		}
	}
}

func TestNoGoTestBindsAFixedPort(t *testing.T) {
	t.Parallel()
	for _, root := range []string{"../../cmd", "../../pkg", "../../internal"} {
		err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
			if err != nil || d.IsDir() || !strings.HasSuffix(path, "_test.go") {
				return err
			}
			if path == filepath.Join("..", "..", "cmd", "vmafx-server", "fixed_port_guard_test.go") {
				return nil // holds the matcher's own examples
			}
			src, readErr := os.ReadFile(path)
			if readErr != nil {
				return readErr
			}
			if m := fixedPortBindRe.Find(src); m != nil {
				t.Errorf("%s binds a fixed port: %s (bind 127.0.0.1:0 and read the address back)", path, m)
			}
			return nil
		})
		if err != nil {
			t.Fatalf("walk %s: %v", root, err)
		}
	}
}
