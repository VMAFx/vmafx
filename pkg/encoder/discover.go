// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/encoder/discover.go — ffmpeg codec availability discovery.
//
// At vmafx-node startup the node runs "ffmpeg -encoders" once and caches
// which codecs are available.  Hardware encoders are then instantiated only
// when the corresponding codec is confirmed present, avoiding hard failures
// when the GPU driver is absent.
//
// ADR-0713: vmafx-node Go worker binary.

package encoder

import (
	"context"
	"os/exec"
	"strings"
	"sync"
	"time"
)

// codecCache caches the result of probeAvailableCodecs.
var (
	codecCacheMu  sync.Mutex
	codecCacheMap map[string]bool
	codecCacheBin string // ffmpeg binary the cached map was probed against
)

// probeAvailableCodecs runs "ffmpeg -encoders" and returns a set of
// available codec names. The result is cached per ffmpeg binary path so
// the second call with the same binary returns instantly; a call with a
// different binary path re-probes and replaces the cache.
//
// Cache invalidation on binary change is the explicit contract here — the
// previous implementation used a one-shot sync.Once and silently returned
// the first-binary's result forever, even when the caller switched to a
// different ffmpeg (e.g. the dev-mcp container's
// /usr/local/bin/ffmpeg-vmaf vs a host /usr/bin/ffmpeg). Stale cache
// would then claim h264_nvenc was "unavailable" because the first probe
// hit a CPU-only build.
func probeAvailableCodecs(ffmpegBin string) map[string]bool {
	codecCacheMu.Lock()
	defer codecCacheMu.Unlock()

	normBin := ffmpegBin
	if normBin == "" {
		normBin = "ffmpeg"
	}
	if codecCacheMap != nil && codecCacheBin == normBin {
		return codecCacheMap
	}
	codecCacheMap = runCodecProbe(normBin)
	codecCacheBin = normBin
	return codecCacheMap
}

// RefreshCodecCache forces a re-probe of codec availability.  Call after
// changing the ffmpeg binary path.  Thread-safe.
func RefreshCodecCache(ffmpegBin string) map[string]bool {
	codecCacheMu.Lock()
	defer codecCacheMu.Unlock()
	normBin := ffmpegBin
	if normBin == "" {
		normBin = "ffmpeg"
	}
	codecCacheMap = runCodecProbe(normBin)
	codecCacheBin = normBin
	return codecCacheMap
}

// runCodecProbe executes "ffmpeg -encoders" and returns the codec name set.
// Bounded by a 30-second timeout so a hung ffmpeg (waiting on a GPU driver
// init, broken network mount, etc.) cannot indefinitely block first-call
// initialisation.
func runCodecProbe(ffmpegBin string) map[string]bool {
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, ffmpegBin, "-encoders", "-hide_banner").Output() //nolint:gosec // ffmpegBin is a tool path
	if err != nil {
		return map[string]bool{}
	}
	result := make(map[string]bool)
	for _, line := range strings.Split(string(out), "\n") {
		// ffmpeg -encoders output format (column 2 is codec name):
		//   VFS... libx264              H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10
		trimmed := strings.TrimSpace(line)
		if len(trimmed) < 8 {
			continue
		}
		// First 6 chars are capability flags; field after is codec name.
		fields := strings.Fields(trimmed)
		if len(fields) >= 2 && len(fields[0]) == 6 {
			result[fields[1]] = true
		}
	}
	return result
}

// IsCodecAvailable returns true if the named codec appears in ffmpeg's encoder
// list.  Runs the probe lazily on first call.
func IsCodecAvailable(codecName string) bool {
	m := probeAvailableCodecs("")
	return m[codecName]
}

// AvailableHardwareEncoders returns the subset of hardware encoder names
// (h264_nvenc, h264_qsv, h264_amf, etc.) that ffmpeg reports as available.
func AvailableHardwareEncoders() []string {
	hw := []string{
		"h264_nvenc", "hevc_nvenc",
		"h264_qsv", "hevc_qsv",
		"h264_amf", "hevc_amf",
	}
	m := probeAvailableCodecs("")
	var available []string
	for _, name := range hw {
		if m[name] {
			available = append(available, name)
		}
	}
	return available
}

// AllKnownEncoders returns all encoder names supported by this package
// (Stage 1 software + Stage 2 hardware stubs).
func AllKnownEncoders() []string {
	return append(KnownEncoders(), hardwareEncoderNames()...)
}

// hardwareEncoderNames returns the hard-coded list of hardware encoder names
// this package can construct (availability not checked).
func hardwareEncoderNames() []string {
	return []string{
		"h264_nvenc", "hevc_nvenc",
		"h264_qsv", "hevc_qsv",
		"h264_amf", "hevc_amf",
		"libsvtav1", "libaom-av1",
	}
}
