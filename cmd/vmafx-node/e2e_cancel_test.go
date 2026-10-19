// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/e2e_cancel_test.go — end-to-end: CancelJob on a real
// vmafx-controller process stops the vmaf process of the node running the
// job (ADR-1567).
//
// The node's vmaf is a script that records its PID and sleeps in place
// (exec), standing in for a long scoring run; the test passes when that
// process is gone after the cancel and the job stays CANCELLED.

//go:build cgo && unix

package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/internal/execstub"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// sleepingVmaf writes a vmaf stand-in that stores its PID in pidFile and
// then sleeps as the same process.
func sleepingVmaf(t *testing.T, pidFile string) string {
	t.Helper()
	bin := filepath.Join(t.TempDir(), "vmaf")
	script := fmt.Sprintf("#!/bin/sh\necho $$ > %q\nexec sleep 300\n", pidFile)
	execstub.Write(t, bin, []byte(script))
	return bin
}

// awaitPID waits for the stand-in to record its PID.
func awaitPID(t *testing.T, pidFile string) int {
	t.Helper()
	deadline := time.Now().Add(60 * time.Second)
	for time.Now().Before(deadline) {
		raw, err := os.ReadFile(pidFile)
		if pid, perr := strconv.Atoi(strings.TrimSpace(string(raw))); err == nil && perr == nil && pid > 0 {
			return pid
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("the node never started vmaf (no PID in %s)", pidFile)
	return 0
}

// awaitExit waits until pid no longer exists.
func awaitExit(t *testing.T, pid int) {
	t.Helper()
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		if err := syscall.Kill(pid, 0); errors.Is(err, syscall.ESRCH) {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("vmaf process %d still runs 30 s after CancelJob", pid)
}

func TestEndToEndCancelStopsTheNodesVmaf(t *testing.T) {
	root := libvmaf.RepoRoot()
	pidFile := filepath.Join(t.TempDir(), "vmaf.pid")
	vmafBin := sleepingVmaf(t, pidFile)
	media := t.TempDir()
	ref, dis := filepath.Join(media, "ref.y4m"), filepath.Join(media, "dis.y4m")
	writeY4M(t, ref, 0)
	writeY4M(t, dis, 40)

	t.Setenv("VMAFX_SCORING_ROOTS", media)
	ctrlAddr := startController(t, root, vmafBin)
	client := controllerv1.NewVmafxControllerClient(dialPlain(t, ctrlAddr))
	app := startE2ENode(t, root, vmafBin, ctrlAddr, map[string]string{
		"VMAFX_CONTROLLER_HEARTBEAT_INTERVAL": "200ms",
	})
	defer app.RequireStop()

	id := submit(t, client, ref, dis, "cpu")
	pid := awaitPID(t, pidFile)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if _, err := client.CancelJob(ctx, &controllerv1.CancelJobRequest{JobId: id}); err != nil {
		t.Fatalf("CancelJob: %v", err)
	}
	awaitExit(t, pid)

	job, err := client.GetJob(ctx, &controllerv1.GetJobRequest{JobId: id})
	if err != nil {
		t.Fatalf("GetJob: %v", err)
	}
	if job.GetStatus() != controllerv1.JobStatus_CANCELLED {
		t.Fatalf("job is %v after the node stopped it, want CANCELLED", job.GetStatus())
	}
}
