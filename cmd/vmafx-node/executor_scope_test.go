// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/executor_scope_test.go — the node scores a controller job
// only when its inputs resolve under the scoring roots sent with it
// (ADR-1577).
//
// Negative: a job without roots, a ".." traversal, another tenant's
// directory and a symlink that leaves the root never reach vmaf. Positive: a
// symlink inside the root is scored by its real path.

//go:build cgo

package main

import (
	"context"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/internal/execstub"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// recordingVmaf writes a vmaf stand-in that stores its arguments in the
// returned file and fails, and an executor that runs it.
func recordingVmaf(t *testing.T) (*Executor, string) {
	t.Helper()
	argsFile := filepath.Join(t.TempDir(), "args")
	bin := filepath.Join(t.TempDir(), "vmaf")
	script := "#!/bin/sh\nprintf '%s\\n' \"$@\" > " + argsFile + "\nexit 1\n"
	execstub.Write(t, bin, []byte(script))
	modelDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(modelDir, "vmaf_v0.6.1.json"), []byte("{}"), 0o600); err != nil {
		t.Fatal(err)
	}
	scorer, err := libvmaf.New(bin, modelDir)
	if err != nil {
		t.Fatal(err)
	}
	return NewExecutor(scorer, nil, "cpu", slog.New(slog.DiscardHandler)), argsFile
}

// scopeTree builds <dir>/acme/{ref,dis}.y4m, <dir>/rival/secret.y4m, the link
// acme/steal.y4m -> rival/secret.y4m and acme/alias.y4m -> acme/ref.y4m.
func scopeTree(t *testing.T) string {
	t.Helper()
	dir, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range []string{"acme/ref.y4m", "acme/dis.y4m", "rival/secret.y4m"} {
		full := filepath.Join(dir, p)
		if err := os.MkdirAll(filepath.Dir(full), 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, nil, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	links := map[string]string{"acme/steal.y4m": "rival/secret.y4m", "acme/alias.y4m": "acme/ref.y4m"}
	for link, target := range links {
		if err := os.Symlink(filepath.Join(dir, target), filepath.Join(dir, link)); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func TestExecutorRefusesInputsOutsideTheJobRoots(t *testing.T) {
	dir := scopeTree(t)
	acme := filepath.Join(dir, "acme")
	cases := map[string]struct {
		ref   string
		roots []string
	}{
		"no roots":     {filepath.Join(acme, "ref.y4m"), nil},
		"traversal":    {acme + "/../rival/secret.y4m", []string{acme}},
		"other tenant": {filepath.Join(dir, "rival", "secret.y4m"), []string{acme}},
		"symlink out":  {filepath.Join(acme, "steal.y4m"), []string{acme}},
	}
	for name, tc := range cases {
		exec, argsFile := recordingVmaf(t)
		res := exec.Execute(context.Background(), &controllerv1.Job{Id: name, Scoring: &controllerv1.ScoringParams{
			Reference: tc.ref, Distorted: filepath.Join(acme, "dis.y4m"), Model: "vmaf_v0.6.1",
		}, ScoringRoots: tc.roots})
		if res.Error == nil || !strings.Contains(res.Error.Error(), "outside the tenant's scoring roots") {
			t.Errorf("%s: error %v, want the scoring-root refusal", name, res.Error)
		}
		if _, err := os.Stat(argsFile); err == nil {
			t.Errorf("%s: vmaf ran on a refused input", name)
		}
	}
}

func TestExecutorScoresALinkInsideTheRootByItsRealPath(t *testing.T) {
	dir := scopeTree(t)
	acme := filepath.Join(dir, "acme")
	exec, argsFile := recordingVmaf(t)
	exec.Execute(context.Background(), &controllerv1.Job{Id: "alias", Scoring: &controllerv1.ScoringParams{
		Reference: filepath.Join(acme, "alias.y4m"), Distorted: filepath.Join(acme, "dis.y4m"), Model: "vmaf_v0.6.1",
	}, ScoringRoots: []string{acme}})
	raw, err := os.ReadFile(argsFile)
	if err != nil {
		t.Fatalf("vmaf did not run: %v", err)
	}
	args := string(raw)
	if !strings.Contains(args, filepath.Join(acme, "ref.y4m")) || strings.Contains(args, "alias.y4m") {
		t.Fatalf("vmaf arguments %q, want the real path of the link", args)
	}
}
