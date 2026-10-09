// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package vmaftest

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/internal/execstub"
)

func writeExe(t *testing.T, path string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
		t.Fatal(err)
	}
	execstub.Write(t, path, []byte("#!/bin/sh\n"))
}

func TestResolve_envWins(t *testing.T) {
	root := t.TempDir()
	writeExe(t, filepath.Join(root, BuildRelPath))
	custom := filepath.Join(t.TempDir(), "vmaf")
	writeExe(t, custom)
	got, err := Resolve(custom, root)
	if err != nil || got != custom {
		t.Fatalf("Resolve(env) = %q, %v; want %q", got, err, custom)
	}
}

func TestResolve_buildDirWhenEnvUnset(t *testing.T) {
	root := t.TempDir()
	want := filepath.Join(root, BuildRelPath)
	writeExe(t, want)
	got, err := Resolve("", root)
	if err != nil || got != want {
		t.Fatalf("Resolve(\"\") = %q, %v; want %q", got, err, want)
	}
}

// A root without a build fails, whatever the host has installed: no third source.
func TestResolve_noBuildFailsEvenWhenHostHasAnInstall(t *testing.T) {
	if _, err := os.Stat("/usr/local/bin/vmaf"); err != nil {
		t.Log("no host /usr/local/bin/vmaf here; the assertion below still holds")
	}
	got, err := Resolve("", t.TempDir())
	if err == nil {
		t.Fatalf("Resolve with no build returned %q; want an error", got)
	}
	if !strings.Contains(err.Error(), "build core/build-cpu") {
		t.Errorf("error does not say how to build the binary: %v", err)
	}
}

func TestResolve_envPointingNowhereFailsWithoutFallingBack(t *testing.T) {
	root := t.TempDir()
	writeExe(t, filepath.Join(root, BuildRelPath)) // a build exists, but VMAF_BIN names another path
	if got, err := Resolve(filepath.Join(root, "missing"), root); err == nil {
		t.Fatalf("Resolve(missing env) returned %q; want an error, not the build dir", got)
	}
}

func TestResolve_rejectsDirectoryAndNonExecutable(t *testing.T) {
	root := t.TempDir()
	if _, err := Resolve(root, root); err == nil {
		t.Error("a directory was accepted")
	}
	plain := filepath.Join(root, "plain")
	if err := os.WriteFile(plain, []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := Resolve(plain, root); err == nil {
		t.Error("a non-executable file was accepted")
	}
}
