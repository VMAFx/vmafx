// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

//go:build cgo

package main

import (
	"context"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"testing"
	"time"
)

func TestParseNvidiaSMI(t *testing.T) {
	t.Parallel()
	got, err := parseNvidiaSMI("0, NVIDIA GeForce RTX 4090, 14352, 24564\n1, NVIDIA L4, 0, 23034\n\n")
	if err != nil {
		t.Fatal(err)
	}
	want := []deviceMemory{
		{device: "0 NVIDIA GeForce RTX 4090", used: 14352 * mib, total: 24564 * mib},
		{device: "1 NVIDIA L4", used: 0, total: 23034 * mib},
	}
	if !slices.Equal(got, want) {
		t.Errorf("parseNvidiaSMI = %+v, want %+v", got, want)
	}
	for _, bad := range []string{"0, name, 1\n", "0, name, x, 2\n", "0, name, 1, [N/A]\n"} {
		if _, err := parseNvidiaSMI(bad); err == nil {
			t.Errorf("parseNvidiaSMI(%q) accepted a malformed line", bad)
		}
	}
}

// TestNvidiaSMIMemoryRunsTheQuery runs a stand-in nvidia-smi that checks its
// arguments, and a missing one, which is an error (counted, not fatal).
func TestNvidiaSMIMemoryRunsTheQuery(t *testing.T) {
	t.Parallel()
	if runtime.GOOS == "windows" {
		t.Skip("the stand-in is a POSIX shell script; parseNvidiaSMI covers the format")
	}
	bin := filepath.Join(t.TempDir(), "nvidia-smi")
	script := "#!/bin/sh\n[ \"$1\" = --query-gpu=index,name,memory.used,memory.total ] || exit 3\n" +
		"[ \"$2\" = --format=csv,noheader,nounits ] || exit 4\necho '0, GPU A, 1, 2'\n"
	if err := os.WriteFile(bin, []byte(script), 0o700); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	got, err := nvidiaSMIMemory(bin)(ctx)
	if err != nil || len(got) != 1 || got[0].total != 2*mib {
		t.Fatalf("read = %+v, %v", got, err)
	}
	if _, err := nvidiaSMIMemory(filepath.Join(t.TempDir(), "absent"))(ctx); err == nil {
		t.Error("a missing nvidia-smi read without an error")
	}
}

// TestAmdgpuMemoryReadsVRAMFiles builds a fake /sys/class/drm: an amdgpu card
// with VRAM files, a card of another driver without them, and a connector.
func TestAmdgpuMemoryReadsVRAMFiles(t *testing.T) {
	t.Parallel()
	root := t.TempDir()
	write := func(rel, content string) {
		p := filepath.Join(root, rel)
		if err := os.MkdirAll(filepath.Dir(p), 0o750); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	write("card2/device/mem_info_vram_used", "16740352\n")
	write("card2/device/mem_info_vram_total", "2147483648\n")
	write("card0/device/vendor", "0x8086\n")
	write("card2-DP-1/status", "disconnected\n")
	got, err := amdgpuMemory(root)(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 || got[0].used != 16740352 || got[0].total != 2147483648 || got[0].device != "card2 device" {
		t.Errorf("amdgpuMemory = %+v", got)
	}
}

func TestDeviceMemoryForBackend(t *testing.T) {
	t.Parallel()
	for backend, want := range map[string]bool{"cuda": true, "hip": true, "sycl": false, "metal": false, "cpu": false} {
		if got := deviceMemoryFor(backend) != nil; got != want {
			t.Errorf("deviceMemoryFor(%q) has a reader = %v, want %v", backend, got, want)
		}
	}
}
