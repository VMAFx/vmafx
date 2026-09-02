// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package probe enumerates the encoder inventory of a local ffmpeg binary.
//
// The vmafx-node startup probe runs `ffmpeg -encoders` once and caches the
// result so the gRPC server can answer "is this codec available?" without
// shelling out on every job request.
//
// The inventory is intentionally immutable after construction — the node does
// not hot-reload codec availability between jobs. A restart is required if the
// host's codec support changes (e.g., GPU driver update enabling NVENC).
package probe

import (
	"context"
	"fmt"
	"log/slog"
	"os/exec"
	"strings"
)

// Inventory holds the cached encoder availability results.
type Inventory struct {
	// Available is the list of ffmpeg encoder names that were found in
	// `ffmpeg -encoders` output.
	Available []string

	// Missing is the list of codec names from the expected set that were
	// NOT found in the output.  Non-empty only when the node is running
	// in an environment where some expected codecs are absent.
	Missing []string
}

// EmptyInventory returns an Inventory with no entries. Used when the probe
// itself fails so the node can still serve with degraded capability.
func EmptyInventory() *Inventory {
	return &Inventory{}
}

// expectedSoftwareCodecs is the set of software encoders every vmafx-node
// image should carry regardless of GPU variant (ADR-0717 §Codec encoder list).
var expectedSoftwareCodecs = []string{
	"libx264",
	"libx265",
	"libvpx-vp9",
	"libsvtav1",
	"libdav1d", // dav1d is a decoder — listed for completeness, not an encoder
}

// EncoderInventory runs `ffmpegBin -encoders` and returns the parsed inventory.
// It also logs a WARN for each expected software codec that is absent.
//
// ctx is forwarded to exec.CommandContext so that a cancelled caller (timeout,
// SIGINT during startup) terminates the ffmpeg subprocess instead of leaking
// it. Pass context.Background() if no caller-side context is available.
func EncoderInventory(ctx context.Context, ffmpegBin string) (*Inventory, error) {
	out, err := exec.CommandContext(ctx, ffmpegBin, "-hide_banner", "-encoders").Output()
	if err != nil {
		return nil, fmt.Errorf("ffmpeg -encoders: %w", err)
	}

	lines := strings.Split(string(out), "\n")
	present := make(map[string]bool, 64)

	for _, line := range lines {
		line = strings.TrimSpace(line)
		// ffmpeg -encoders output format:
		//  V..... libx264              libx264 H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10
		//  ^      ^-- codec name starts at column 8
		if len(line) < 8 {
			continue
		}
		// Skip header rows (no leading type flags).
		if line[0] == '-' || line[0] == '=' || strings.HasPrefix(line, "Encoders:") {
			continue
		}
		// The flags field is 6 chars wide followed by a space, then the name.
		// We split and take the second token as the codec name.
		fields := strings.Fields(line)
		if len(fields) >= 2 {
			present[fields[1]] = true
		}
	}

	var available, missing []string

	for _, codec := range expectedSoftwareCodecs {
		if present[codec] {
			available = append(available, codec)
		} else {
			missing = append(missing, codec)
			slog.Warn("expected codec missing from ffmpeg -encoders",
				"codec", codec, "ffmpeg", ffmpegBin)
		}
	}

	// Also collect all other encoders the binary reports.
	for name := range present {
		found := false
		for _, exp := range expectedSoftwareCodecs {
			if exp == name {
				found = true
				break
			}
		}
		if !found {
			available = append(available, name)
		}
	}

	return &Inventory{
		Available: available,
		Missing:   missing,
	}, nil
}

// Has returns true if the named codec was found in the inventory.
func (inv *Inventory) Has(codec string) bool {
	for _, c := range inv.Available {
		if c == codec {
			return true
		}
	}
	return false
}
