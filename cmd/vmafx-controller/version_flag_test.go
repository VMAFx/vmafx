// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/version_flag_test.go — `vmafx-controller --version`
// prints the release and exits without configuration or listeners, as the
// image smoke test and the docs expect (ADR-1589).

//go:build cgo

package main

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

func TestIsVersionRequest(t *testing.T) {
	if !isVersionRequest([]string{"vmafx-controller", "--version"}) {
		t.Fatal("--version must select the version path")
	}
	for _, args := range [][]string{{"vmafx-controller"}, {"vmafx-controller", "--version", "extra"}, {"vmafx-controller", "-version"}} {
		if isVersionRequest(args) {
			t.Errorf("%q selected the version path", args)
		}
	}
}

func TestVersionFlagPrintsTheReleaseAndExits(t *testing.T) {
	bin := filepath.Join(t.TempDir(), "vmafx-controller")
	build := exec.Command("go", "build", "-o", bin,
		"-ldflags", "-X github.com/VMAFx/vmafx/pkg/version.version=v9.8.7", "./cmd/vmafx-controller")
	build.Dir = libvmaf.RepoRoot()
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build: %v\n%s", err, out)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	// No auth settings in the environment: started for real, the controller
	// would refuse to start; --version must not get that far. The loader's
	// search path stays (loaderEnv).
	cmd := exec.CommandContext(ctx, bin, "--version")
	cmd.Env = append([]string{"PATH=/usr/bin:/bin"}, loaderEnv()...)
	out, err := cmd.Output()
	if err != nil || strings.TrimSpace(string(out)) != "v9.8.7" {
		var stderr []byte
		if exitErr, ok := errors.AsType[*exec.ExitError](err); ok {
			stderr = exitErr.Stderr
		}
		t.Fatalf("--version = %q, %v (stderr %q); want v9.8.7 and exit 0", out, err, stderr)
	}
}

// loaderEnv returns the dynamic loader's search variables of the test's own
// environment. The binary links the build tree's shared libvmaf
// (CGO_LDFLAGS=-L core/build-cpu/src), which the loader finds only through
// them: without them the binary does not start (exit 127) on a host with no
// installed libvmaf, such as the CI runner, and loads the installed one on a
// host that has it.
func loaderEnv() []string {
	var env []string
	for _, name := range []string{"LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH"} {
		if value, ok := os.LookupEnv(name); ok {
			env = append(env, name+"="+value)
		}
	}
	return env
}
