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
	"os/exec"
	"strings"
	"sync"
)

// codecCache caches the result of probeAvailableCodecs.
var (
	codecCacheMu   sync.Mutex
	codecCacheOnce sync.Once
	codecCacheMap  map[string]bool
)

// probeAvailableCodecs runs "ffmpeg -encoders" once and returns a set of
// available codec names.  Subsequent calls return the cached result.
func probeAvailableCodecs(ffmpegBin string) map[string]bool {
	codecCacheMu.Lock()
	defer codecCacheMu.Unlock()

	// Reset cache if bin changes (e.g. in tests).
	_ = ffmpegBin
	codecCacheOnce.Do(func() {
		codecCacheMap = runCodecProbe(ffmpegBin)
	})
	return codecCacheMap
}

// RefreshCodecCache forces a re-probe of codec availability.  Call after
// changing the ffmpeg binary path.  Thread-safe.
func RefreshCodecCache(ffmpegBin string) map[string]bool {
	codecCacheMu.Lock()
	defer codecCacheMu.Unlock()
	codecCacheOnce = sync.Once{} // reset so the next probeAvailableCodecs call re-runs
	result := runCodecProbe(ffmpegBin)
	codecCacheMap = result
	return result
}

// runCodecProbe executes "ffmpeg -encoders" and returns the codec name set.
func runCodecProbe(ffmpegBin string) map[string]bool {
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	out, err := exec.Command(ffmpegBin, "-encoders", "-hide_banner").Output() //nolint:gosec // ffmpegBin is a tool path
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
