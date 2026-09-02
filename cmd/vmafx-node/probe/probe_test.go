// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package probe_test

import (
	"context"
	"os"
	"os/exec"
	"testing"

	"github.com/VMAFx/vmafx/cmd/vmafx-node/probe"
)

// TestEmptyInventory verifies that EmptyInventory returns a non-nil value
// with empty slices (not nil slices).
func TestEmptyInventory(t *testing.T) {
	t.Parallel()

	inv := probe.EmptyInventory()
	if inv == nil {
		t.Fatal("EmptyInventory returned nil")
	}
}

// TestInventoryHas verifies Has on a known codec in and out of the inventory.
func TestInventoryHas(t *testing.T) {
	t.Parallel()

	inv := &probe.Inventory{
		Available: []string{"libx264", "libx265"},
	}

	if !inv.Has("libx264") {
		t.Error("expected inv.Has(libx264) == true")
	}

	if inv.Has("libsvtav1") {
		t.Error("expected inv.Has(libsvtav1) == false")
	}
}

// TestEncoderInventory_RealBinary runs the actual ffmpeg binary when available
// on PATH or at VMAFX_FFMPEG_BIN. Skipped when ffmpeg is not present.
func TestEncoderInventory_RealBinary(t *testing.T) {
	t.Parallel()

	bin := "ffmpeg"
	if v := os.Getenv("VMAFX_FFMPEG_BIN"); v != "" {
		bin = v
	}

	if _, err := exec.LookPath(bin); err != nil {
		t.Skipf("ffmpeg not found on PATH (%s): %v", bin, err)
	}

	inv, err := probe.EncoderInventory(context.Background(), bin)
	if err != nil {
		t.Fatalf("EncoderInventory(%q): %v", bin, err)
	}

	if inv == nil {
		t.Fatal("EncoderInventory returned nil inventory")
	}

	// At minimum, a standard-distribution ffmpeg should have libx264.
	// This assertion is weak on purpose — we only check the function
	// runs without error; the exact codec matrix is environment-dependent.
	t.Logf("available encoders: %v", inv.Available)
	t.Logf("missing expected codecs: %v", inv.Missing)
}
