// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

//go:build cgo

package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/VMAFx/vmafx/pkg/observability"
	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// deviceMemory is the memory of one GPU in bytes.
type deviceMemory struct {
	device      string
	used, total float64
}

// deviceMemoryReader reads the memory of the node's GPUs; ctx bounds the
// read (observability.ScrapeTimeout).
type deviceMemoryReader func(ctx context.Context) ([]deviceMemory, error)

// mib is one mebibyte: nvidia-smi reports memory in MiB.
const mib = 1 << 20

// maxDevices bounds the devices one read reports (HISS-02); the device label
// keeps at most metricdef.DeviceLimit of them.
const maxDevices = 64

// deviceMemoryFor returns the reader for the node's backend, or nil when the
// node has no source for it: cuda reads nvidia-smi, hip the amdgpu sysfs
// files. The Intel xe driver and Metal expose no device memory counter an
// unprivileged process can read; a vendor exporter covers them.
func deviceMemoryFor(backend string) deviceMemoryReader {
	switch backend {
	case "cuda":
		return nvidiaSMIMemory("nvidia-smi")
	case "hip":
		return amdgpuMemory("/sys/class/drm")
	default:
		return nil
	}
}

// nvidiaSMIMemory runs nvidia-smi's query mode, one line per GPU:
// "0, NVIDIA GeForce RTX 4090, 14352, 24564" (index, name, used MiB, total MiB).
func nvidiaSMIMemory(bin string) deviceMemoryReader {
	return func(ctx context.Context) ([]deviceMemory, error) {
		// #nosec G204 -- a fixed binary name and fixed arguments, no caller input.
		out, err := exec.CommandContext(ctx, bin,
			"--query-gpu=index,name,memory.used,memory.total", "--format=csv,noheader,nounits").Output()
		if err != nil {
			return nil, fmt.Errorf("nvidia-smi: %w", err)
		}
		return parseNvidiaSMI(string(out))
	}
}

func parseNvidiaSMI(out string) ([]deviceMemory, error) {
	var devs []deviceMemory
	sc := bufio.NewScanner(strings.NewReader(out))
	for sc.Scan() && len(devs) < maxDevices {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		f := strings.Split(line, ",")
		if len(f) != 4 {
			return nil, fmt.Errorf("nvidia-smi: unexpected line %q", line)
		}
		used, uerr := strconv.ParseFloat(strings.TrimSpace(f[2]), 64)
		total, terr := strconv.ParseFloat(strings.TrimSpace(f[3]), 64)
		if err := errors.Join(uerr, terr); err != nil {
			return nil, fmt.Errorf("nvidia-smi: line %q: %w", line, err)
		}
		devs = append(devs, deviceMemory{
			device: strings.TrimSpace(f[0]) + " " + strings.TrimSpace(f[1]),
			used:   used * mib, total: total * mib,
		})
	}
	return devs, sc.Err()
}

// amdgpuMemory reads mem_info_vram_used and mem_info_vram_total (bytes) of
// every card the amdgpu driver drives under root.
func amdgpuMemory(root string) deviceMemoryReader {
	return func(ctx context.Context) ([]deviceMemory, error) {
		cards, err := filepath.Glob(filepath.Join(root, "card[0-9]*"))
		if err != nil {
			return nil, err
		}
		var devs []deviceMemory
		for _, card := range cards {
			if ctx.Err() != nil || len(devs) >= maxDevices {
				break
			}
			if d, ok := amdgpuCard(card); ok {
				devs = append(devs, d)
			}
		}
		return devs, ctx.Err()
	}
}

// amdgpuCard reads one card; ok is false for a card of another driver or a
// connector entry (card0-DP-1), which have no VRAM files.
func amdgpuCard(card string) (deviceMemory, bool) {
	dev := filepath.Join(card, "device")
	used, uerr := readUint(filepath.Join(dev, "mem_info_vram_used"))
	total, terr := readUint(filepath.Join(dev, "mem_info_vram_total"))
	if uerr != nil || terr != nil {
		return deviceMemory{}, false
	}
	pci := filepath.Base(resolvedPath(dev))
	return deviceMemory{device: filepath.Base(card) + " " + pci, used: float64(used), total: float64(total)}, true
}

func readUint(path string) (uint64, error) {
	b, err := os.ReadFile(path) // #nosec G304 -- a sysfs path under the fixed DRM root
	if err != nil {
		return 0, err
	}
	return strconv.ParseUint(strings.TrimSpace(string(b)), 10, 64)
}

func resolvedPath(p string) string {
	if r, err := filepath.EvalSymlinks(p); err == nil {
		return r
	}
	return p
}

// deviceMemoryScrape turns a reader into the scraped device memory families.
func deviceMemoryScrape(read deviceMemoryReader) observability.ScrapeFunc {
	return func(ctx context.Context) ([]observability.Sample, error) {
		devs, err := read(ctx)
		if err != nil {
			return nil, err
		}
		out := make([]observability.Sample, 0, 2*len(devs))
		for _, d := range devs {
			lv := []string{d.device}
			out = append(out,
				observability.Sample{Family: metricdef.NodeDeviceMemoryUsed, Value: d.used, Labels: lv},
				observability.Sample{Family: metricdef.NodeDeviceMemoryTotal, Value: d.total, Labels: lv})
		}
		return out, nil
	}
}
