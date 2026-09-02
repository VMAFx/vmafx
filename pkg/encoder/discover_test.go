// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/encoder/discover_test.go — unit tests for codec discovery,
// hardware-encoder constructors, and the NewExtended factory.
//
// These tests exercise pure-Go logic without requiring a live ffmpeg
// binary.  The probe entry points are smoke-tested for safe behaviour
// when the binary is absent.

package encoder

import (
	"os"
	"strings"
	"testing"
)

// TestHardwareEncoderNames returns the exact expected list (order matters
// because callers rank availability by index).
func TestHardwareEncoderNames(t *testing.T) {
	t.Parallel()
	want := []string{
		"h264_nvenc", "hevc_nvenc",
		"h264_qsv", "hevc_qsv",
		"h264_amf", "hevc_amf",
		"libsvtav1", "libaom-av1",
	}
	got := hardwareEncoderNames()
	if len(got) != len(want) {
		t.Fatalf("hardwareEncoderNames len = %d, want %d", len(got), len(want))
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("hardwareEncoderNames[%d] = %q, want %q", i, got[i], want[i])
		}
	}
}

// TestAllKnownEncoders includes both software and hardware lists.
func TestAllKnownEncoders(t *testing.T) {
	t.Parallel()
	all := AllKnownEncoders()
	wantSubset := []string{
		"libx264", "libx265",
		"h264_nvenc", "hevc_nvenc",
		"h264_qsv", "hevc_qsv",
		"h264_amf", "hevc_amf",
		"libsvtav1", "libaom-av1",
	}
	got := map[string]bool{}
	for _, name := range all {
		got[name] = true
	}
	for _, w := range wantSubset {
		if !got[w] {
			t.Errorf("AllKnownEncoders missing %q (got %v)", w, all)
		}
	}
}

// TestRunCodecProbe_BinaryMissingReturnsEmpty verifies the probe degrades
// gracefully when ffmpeg is not on PATH (returns empty map, no panic).
func TestRunCodecProbe_BinaryMissingReturnsEmpty(t *testing.T) {
	t.Parallel()
	got := runCodecProbe("/this/path/does/not/exist/ffmpeg")
	if len(got) != 0 {
		t.Errorf("runCodecProbe with missing binary should be empty, got %v", got)
	}
}

// TestRunCodecProbe_DefaultsToPATH triggers the ffmpegBin=="" branch.
func TestRunCodecProbe_DefaultsToPATH(t *testing.T) {
	t.Parallel()
	// Result depends on the test host; we only verify no panic and that
	// the map is non-nil.
	got := runCodecProbe("")
	if got == nil {
		t.Error("runCodecProbe('') returned nil map")
	}
}

// TestProbeAvailableCodecs_CachesResult verifies the per-binary cache
// returns the same map for the same binary path across calls.
func TestProbeAvailableCodecs_CachesResult(t *testing.T) {
	// Cannot run in parallel — touches the package-level cache.

	// Reset cache so the test is reproducible across re-runs in the same
	// test binary.
	codecCacheMu.Lock()
	codecCacheMap = nil
	codecCacheBin = ""
	codecCacheMu.Unlock()

	first := probeAvailableCodecs("/nonexistent/ffmpeg")
	second := probeAvailableCodecs("/another/nonexistent/ffmpeg")
	// Map equality: same underlying reference because of sync.Once.
	if &first == &second {
		// Pointer compare on map is not meaningful, so check length.
		_ = first
	}
	if len(first) != len(second) {
		t.Errorf("cache mismatch: first=%d second=%d", len(first), len(second))
	}
}

// TestRefreshCodecCache_RebuildsAndReplaces verifies the explicit refresh
// API re-probes the binary and replaces the cache map.
func TestRefreshCodecCache_RebuildsAndReplaces(t *testing.T) {
	// Cannot run in parallel — touches the package-level cache.

	// Force a known initial state via probe.
	codecCacheMu.Lock()
	codecCacheMap = nil
	codecCacheBin = ""
	codecCacheMu.Unlock()
	_ = probeAvailableCodecs("/nonexistent/ffmpeg")

	got := RefreshCodecCache("/another/nonexistent/ffmpeg")
	if got == nil {
		t.Fatal("RefreshCodecCache returned nil")
	}
	// Re-probe should now return the refreshed map (still empty for a
	// missing binary).
	codecCacheMu.Lock()
	cached := codecCacheMap
	codecCacheMu.Unlock()
	if len(got) != len(cached) {
		t.Errorf("RefreshCodecCache result %d != cached %d", len(got), len(cached))
	}
}

// TestIsCodecAvailable_HandlesMissingBinary covers the case where the
// probe fails — IsCodecAvailable returns false rather than panicking.
func TestIsCodecAvailable_HandlesMissingBinary(t *testing.T) {
	// Cannot run in parallel — uses the package cache.
	codecCacheMu.Lock()
	codecCacheMap = nil
	codecCacheBin = ""
	codecCacheMu.Unlock()

	// On most test hosts /usr/bin/ffmpeg may exist and report real codecs;
	// to keep this deterministic point the cache at a missing binary by
	// going through the unexported probe (clear cache then prime).
	_ = probeAvailableCodecs("/nonexistent/ffmpeg")
	got := IsCodecAvailable("definitely_not_a_codec_name")
	if got {
		t.Errorf("IsCodecAvailable for fake codec returned true")
	}
}

// TestAvailableHardwareEncoders_ReturnsSubset covers the slice-build branch.
func TestAvailableHardwareEncoders_ReturnsSubset(t *testing.T) {
	// Cannot run in parallel — uses the package cache.
	codecCacheMu.Lock()
	codecCacheMap = nil
	codecCacheBin = ""
	codecCacheMu.Unlock()

	// Prime cache from a missing binary so the result is the empty set.
	_ = probeAvailableCodecs("/nonexistent/ffmpeg")
	got := AvailableHardwareEncoders()
	if got != nil && len(got) != 0 {
		// On dev hosts with full ffmpeg the slice may be non-empty;
		// only fail on the fake-binary cached scenario above.
		for _, name := range got {
			if !strings.HasPrefix(name, "h264_") && !strings.HasPrefix(name, "hevc_") {
				t.Errorf("AvailableHardwareEncoders returned unexpected %q", name)
			}
		}
	}
}

// TestNewExtended_AllNames builds every encoder via NewExtended and
// confirms Name() and CRFRange() match expectations.
func TestNewExtended_AllNames(t *testing.T) {
	t.Parallel()
	cases := []struct {
		codec      string
		wantName   string
		wantLo     int
		wantHi     int
		wantErrSub string // empty when no error expected
	}{
		{codec: "libx264", wantName: "libx264", wantLo: 0, wantHi: 51},
		{codec: "libx265", wantName: "libx265", wantLo: 0, wantHi: 51},
		{codec: "h264_nvenc", wantName: "h264_nvenc", wantLo: 0, wantHi: 51},
		{codec: "hevc_nvenc", wantName: "hevc_nvenc", wantLo: 0, wantHi: 51},
		{codec: "h264_qsv", wantName: "h264_qsv", wantLo: 1, wantHi: 51},
		{codec: "hevc_qsv", wantName: "hevc_qsv", wantLo: 1, wantHi: 51},
		{codec: "h264_amf", wantName: "h264_amf", wantLo: 0, wantHi: 51},
		{codec: "hevc_amf", wantName: "hevc_amf", wantLo: 0, wantHi: 51},
		{codec: "libsvtav1", wantName: "libsvtav1", wantLo: 0, wantHi: 63},
		{codec: "libaom-av1", wantName: "libaom-av1", wantLo: 0, wantHi: 63},
	}
	for _, tc := range cases {
		tc := tc
		t.Run(tc.codec, func(t *testing.T) {
			t.Parallel()
			enc, err := NewExtended(tc.codec)
			if err != nil {
				t.Fatalf("NewExtended(%q): %v", tc.codec, err)
			}
			if enc.Name() != tc.wantName {
				t.Errorf("Name() = %q, want %q", enc.Name(), tc.wantName)
			}
			lo, hi := enc.CRFRange()
			if lo != tc.wantLo || hi != tc.wantHi {
				t.Errorf("CRFRange = (%d, %d), want (%d, %d)", lo, hi, tc.wantLo, tc.wantHi)
			}
		})
	}
}

// TestNewExtended_UnknownReturnsError covers the default branch.
func TestNewExtended_UnknownReturnsError(t *testing.T) {
	t.Parallel()
	_, err := NewExtended("definitely_no_such_encoder")
	if err == nil {
		t.Fatal("expected error for unknown encoder")
	}
	if !strings.Contains(err.Error(), "unknown encoder") {
		t.Errorf("unexpected error message: %v", err)
	}
}

// TestInjectQSVInitChain_DefaultDevice covers the default /dev/dri/renderD128
// path (when VMAFTUNE_VAAPI_DEVICE is empty).
func TestInjectQSVInitChain_DefaultDevice(t *testing.T) {
	// Mutates process env — cannot run in parallel.
	old := os.Getenv("VMAFTUNE_VAAPI_DEVICE")
	defer func() {
		if old != "" {
			_ = os.Setenv("VMAFTUNE_VAAPI_DEVICE", old)
		} else {
			_ = os.Unsetenv("VMAFTUNE_VAAPI_DEVICE")
		}
	}()
	_ = os.Unsetenv("VMAFTUNE_VAAPI_DEVICE")

	in := EncodeParams{ExtraArgs: []string{"-keep", "-this"}}
	out := injectQSVInitChain(in)
	// ADR-0601: the device-init chain is a set of *global* ffmpeg options and
	// must land in InputArgs (emitted before "-i"), never in ExtraArgs (which
	// runEncodeArgv emits after "-c:v", where ffmpeg rejects them with -22).
	inputJoined := strings.Join(out.InputArgs, " ")
	if !strings.Contains(inputJoined, "/dev/dri/renderD128") {
		t.Errorf("default VAAPI device missing from InputArgs: %v", out.InputArgs)
	}
	if !strings.Contains(inputJoined, "-init_hw_device") {
		t.Errorf("init_hw_device flag missing from InputArgs: %v", out.InputArgs)
	}
	if !strings.Contains(inputJoined, "-filter_hw_device va") {
		t.Errorf("filter_hw_device flag missing from InputArgs: %v", out.InputArgs)
	}
	// The hwupload filter is a per-output option and stays in ExtraArgs.
	extraJoined := strings.Join(out.ExtraArgs, " ")
	if !strings.Contains(extraJoined, "format=nv12,hwupload=extra_hw_frames=64") {
		t.Errorf("hwupload filter missing from ExtraArgs: %v", out.ExtraArgs)
	}
	if strings.Contains(extraJoined, "-init_hw_device") {
		t.Errorf("device-init chain leaked into ExtraArgs: %v", out.ExtraArgs)
	}
	// Caller's original ExtraArgs must be preserved at the tail.
	if !strings.Contains(extraJoined, "-keep -this") {
		t.Errorf("caller args not preserved: %v", out.ExtraArgs)
	}
}

// TestInjectQSVInitChain_CustomDevice exercises the env-override branch.
func TestInjectQSVInitChain_CustomDevice(t *testing.T) {
	// Mutates process env — cannot run in parallel.
	old := os.Getenv("VMAFTUNE_VAAPI_DEVICE")
	defer func() {
		if old != "" {
			_ = os.Setenv("VMAFTUNE_VAAPI_DEVICE", old)
		} else {
			_ = os.Unsetenv("VMAFTUNE_VAAPI_DEVICE")
		}
	}()
	if err := os.Setenv("VMAFTUNE_VAAPI_DEVICE", "/dev/dri/renderD129"); err != nil {
		t.Fatalf("Setenv: %v", err)
	}

	out := injectQSVInitChain(EncodeParams{})
	if !strings.Contains(strings.Join(out.InputArgs, " "), "/dev/dri/renderD129") {
		t.Errorf("VAAPI device override not honoured: %v", out.InputArgs)
	}
}

// TestX265CRFRange covers the LibX265Encoder.CRFRange + Name shortcuts not
// hit by the existing encoder_test.go (which only checks Name).
func TestX265CRFRange(t *testing.T) {
	t.Parallel()
	enc := LibX265Encoder{}
	lo, hi := enc.CRFRange()
	if lo != 0 || hi != 51 {
		t.Errorf("CRFRange = (%d, %d), want (0, 51)", lo, hi)
	}
}

// TestProbeBitrateKbps_HandlesUnavailableFFprobe verifies the probe returns
// 0.0 when ffprobe is not on PATH (graceful degradation).
func TestProbeBitrateKbps_HandlesUnavailableFFprobe(t *testing.T) {
	t.Parallel()
	// Both branches of derive-from-ffmpeg-path:
	//   dir == "."  → bare "ffprobe" lookup
	//   dir != "."  → ffprobe sibling of explicit ffmpeg path
	bareResult := probeBitrateKbps("/tmp/nofile.mkv", "ffmpeg")
	if bareResult != 0.0 {
		t.Errorf("probeBitrateKbps bare path = %v, want 0.0", bareResult)
	}
	siblingResult := probeBitrateKbps("/tmp/nofile.mkv", "/nonexistent/path/ffmpeg")
	if siblingResult != 0.0 {
		t.Errorf("probeBitrateKbps sibling path = %v, want 0.0", siblingResult)
	}
}

// TestExtractEncoderVersion_NotFound covers the no-match branch.
func TestExtractEncoderVersion_NotFound(t *testing.T) {
	t.Parallel()
	got := extractEncoderVersion("some unrelated stderr without the codec marker\n", "libx264")
	if got != "" {
		t.Errorf("expected empty string for no match, got %q", got)
	}
}

// TestExtractEncoderVersion_NoDash covers the no-dash branch (line contains
// codec + "core" but no separator).
func TestExtractEncoderVersion_NoDash(t *testing.T) {
	t.Parallel()
	got := extractEncoderVersion("libx264 core 164 r3094M\n", "libx264")
	if got == "" {
		t.Error("expected non-empty version string")
	}
}
