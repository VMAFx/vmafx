// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/controller_graph_test.go — the controller client inside the
// production fx graph: startup refusals, the stop order with the client
// enabled, and the backend the executor hands to the vmaf CLI.

//go:build cgo

package main

import (
	"context"
	"log/slog"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"

	"go.uber.org/fx"
	"go.uber.org/fx/fxevent"
	"go.uber.org/fx/fxtest"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/internal/execstub"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// TestStopOrderNodeWithController: with the client enabled, fx stops the gRPC
// server first, then drains the controller client, then the FeedbackClient,
// then the scorer.
func TestStopOrderNodeWithController(t *testing.T) {
	writeNodeEnv(t)
	f := newFakeController()
	t.Setenv("VMAFX_CONTROLLER_ADDR", serveFake(t, f))
	t.Setenv("VMAFX_NODE_ID", "graph-node")

	rec := &orderRecorder{}
	app := fxtest.New(t, productionGraph(), fx.WithLogger(func() fxevent.Logger { return rec }))
	app.RequireStart()
	deadline := time.Now().Add(5 * time.Second)
	for f.callCount("RegisterNode") == 0 && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if f.callCount("RegisterNode") == 0 {
		t.Fatal("the node never registered with the controller")
	}
	app.RequireStop()

	rec.mu.Lock()
	defer rec.mu.Unlock()
	idx := func(part string) int {
		return slices.IndexFunc(rec.order, func(c string) bool { return strings.Contains(c, part) })
	}
	grpcIdx, ctrlIdx := idx("golusoris/grpc"), idx("provideControllerClient")
	feedbackIdx, scorerIdx := idx("provideFeedbackClient"), idx("provideScorer")
	if grpcIdx < 0 || ctrlIdx < 0 || feedbackIdx < 0 || scorerIdx < 0 {
		t.Fatalf("missing OnStop hooks; callers=%v", rec.order)
	}
	if grpcIdx >= ctrlIdx || ctrlIdx >= feedbackIdx || feedbackIdx >= scorerIdx {
		t.Fatalf("stop order grpc=%d controller=%d feedback=%d scorer=%d, want ascending; callers=%v",
			grpcIdx, ctrlIdx, feedbackIdx, scorerIdx, rec.order)
	}
}

// TestControllerClientRefusedWithoutScorer: a node that cannot score must not
// pull jobs, so the graph refuses to start (negative).
func TestControllerClientRefusedWithoutScorer(t *testing.T) {
	writeNodeEnv(t)
	t.Setenv("VMAFX_VMAF_BINARY", filepath.Join(t.TempDir(), "no-such-vmaf"))
	t.Setenv("VMAFX_CONTROLLER_ADDR", "127.0.0.1:1")
	t.Setenv("VMAFX_NODE_ID", "graph-node")
	err := fx.New(productionGraph(), fx.NopLogger).Err()
	if err == nil || !strings.Contains(err.Error(), "must not pull jobs") {
		t.Fatalf("graph error = %v, want the no-scorer refusal", err)
	}
}

// TestControllerClientRefusesUnadvertisableBackend: VMAFX_BACKEND=auto cannot
// be matched by the scheduler, so the graph refuses it (negative).
func TestControllerClientRefusesUnadvertisableBackend(t *testing.T) {
	writeNodeEnv(t)
	t.Setenv("VMAFX_BACKEND", "auto")
	t.Setenv("VMAFX_CONTROLLER_ADDR", "127.0.0.1:1")
	t.Setenv("VMAFX_NODE_ID", "graph-node")
	err := fx.New(productionGraph(), fx.NopLogger).Err()
	if err == nil || !strings.Contains(err.Error(), "cannot be advertised") {
		t.Fatalf("graph error = %v, want the backend refusal", err)
	}
}

// TestControllerClientRefusesBadConfig: a malformed controller setting fails
// startup instead of falling back to a default (negative).
func TestControllerClientRefusesBadConfig(t *testing.T) {
	writeNodeEnv(t)
	t.Setenv("VMAFX_CONTROLLER_ADDR", "127.0.0.1:1")
	t.Setenv("VMAFX_CONTROLLER_RPC_TIMEOUT", "soon")
	err := fx.New(productionGraph(), fx.NopLogger).Err()
	if err == nil || !strings.Contains(err.Error(), "controller.rpc_timeout") {
		t.Fatalf("graph error = %v, want the rpc_timeout refusal", err)
	}
}

// TestControllerClientDisabledWithoutAddr: without an address the graph starts
// and provides no client (boundary).
func TestControllerClientDisabledWithoutAddr(t *testing.T) {
	writeNodeEnv(t)
	t.Setenv("VMAFX_CONTROLLER_ADDR", "")
	var client *controllerClient
	app := fxtest.New(t, productionGraph(), fx.Populate(&client))
	app.RequireStart()
	app.RequireStop()
	if client != nil {
		t.Fatal("a controller client was built without VMAFX_CONTROLLER_ADDR")
	}
}

// TestExecuteScoring_PassesBackend: the vmaf CLI receives the job's backend,
// or the node's when the job names none, as --backend.
func TestExecuteScoring_PassesBackend(t *testing.T) {
	for jobBackendName, want := range map[string]string{"": "cpu", "cuda": "cuda"} {
		argsFile := filepath.Join(t.TempDir(), "args")
		script := "#!/bin/sh\nprintf '%s\\n' \"$@\" > " + argsFile + "\nexit 1\n"
		bin := filepath.Join(t.TempDir(), "vmaf")
		execstub.Write(t, bin, []byte(script))
		modelDir := t.TempDir()
		if err := os.WriteFile(filepath.Join(modelDir, "vmaf_v0.6.1.json"), []byte("{}"), 0o600); err != nil {
			t.Fatal(err)
		}
		scorer, err := libvmaf.New(bin, modelDir)
		if err != nil {
			t.Fatal(err)
		}
		media := t.TempDir()
		for _, name := range []string{"r.y4m", "d.y4m"} {
			if err := os.WriteFile(filepath.Join(media, name), nil, 0o600); err != nil {
				t.Fatal(err)
			}
		}
		job := &controllerv1.Job{Id: "b", Scoring: &controllerv1.ScoringParams{
			Reference: filepath.Join(media, "r.y4m"), Distorted: filepath.Join(media, "d.y4m"),
			Model: "vmaf_v0.6.1", Backend: jobBackendName,
		}, ScoringRoots: []string{media}}
		NewExecutor(scorer, nil, "cpu", slog.New(slog.DiscardHandler)).Execute(context.Background(), job)
		raw, err := os.ReadFile(argsFile)
		if err != nil {
			t.Fatalf("vmaf stub did not run: %v", err)
		}
		args := strings.Split(strings.TrimSpace(string(raw)), "\n")
		i := slices.Index(args, "--backend")
		if i < 0 || i+1 >= len(args) || args[i+1] != want {
			t.Errorf("job backend %q: argv %q, want --backend %s", jobBackendName, args, want)
		}
	}
}
