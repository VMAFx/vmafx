// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/backend/sweep_test.go — registering the lease sweep
// worker reports the failures golusoris jobs.Register returns (v0.13.0).

package backend_test

import (
	"testing"

	"github.com/golusoris/golusoris/jobs"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/backend"
)

func newSweeper() *backend.LeaseSweeper {
	return backend.NewLeaseSweeper(nil, backend.ExponentialBackoff(0, 0), nil)
}

func TestRegisterLeaseSweepAcceptsFreshWorkers(t *testing.T) {
	t.Parallel()
	if err := backend.RegisterLeaseSweep(jobs.NewWorkers(), newSweeper()); err != nil {
		t.Fatalf("register on fresh workers: %v", err)
	}
}

func TestRegisterLeaseSweepRefusesMissingWorkersOrSweeper(t *testing.T) {
	t.Parallel()
	if err := backend.RegisterLeaseSweep(nil, newSweeper()); err == nil {
		t.Fatal("nil workers registered the sweep")
	}
	if err := backend.RegisterLeaseSweep(&jobs.Workers{}, newSweeper()); err == nil {
		t.Fatal("uninitialised workers registered the sweep")
	}
	if err := backend.RegisterLeaseSweep(jobs.NewWorkers(), nil); err == nil {
		t.Fatal("a nil sweeper was registered")
	}
}

func TestRegisterLeaseSweepRefusesASecondRegistration(t *testing.T) {
	t.Parallel()
	workers := jobs.NewWorkers()
	if err := backend.RegisterLeaseSweep(workers, newSweeper()); err != nil {
		t.Fatalf("first registration: %v", err)
	}
	if err := backend.RegisterLeaseSweep(workers, newSweeper()); err == nil {
		t.Fatal("the sweep kind was registered twice")
	}
}
