// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package encoder_test

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/VMAFx/vmafx/pkg/encoder"
)

func TestKnownEncoders(t *testing.T) {
	t.Parallel()
	known := encoder.KnownEncoders()
	if len(known) < 2 {
		t.Fatalf("expected at least 2 known encoders, got %d", len(known))
	}
	found := map[string]bool{}
	for _, name := range known {
		found[name] = true
	}
	for _, required := range []string{"libx264", "libx265"} {
		if !found[required] {
			t.Errorf("expected %q in KnownEncoders, got %v", required, known)
		}
	}
}

func TestNew_unknown(t *testing.T) {
	t.Parallel()
	_, err := encoder.New("nosuch_encoder")
	if err == nil {
		t.Fatal("expected error for unknown encoder, got nil")
	}
}

func TestNew_x264(t *testing.T) {
	t.Parallel()
	enc, err := encoder.New("libx264")
	if err != nil {
		t.Fatalf("New(libx264): %v", err)
	}
	if enc.Name() != "libx264" {
		t.Errorf("Name() = %q, want %q", enc.Name(), "libx264")
	}
	lo, hi := enc.CRFRange()
	if lo != 0 || hi != 51 {
		t.Errorf("CRFRange() = (%d, %d), want (0, 51)", lo, hi)
	}
}

func TestNew_x265(t *testing.T) {
	t.Parallel()
	enc, err := encoder.New("libx265")
	if err != nil {
		t.Fatalf("New(libx265): %v", err)
	}
	if enc.Name() != "libx265" {
		t.Errorf("Name() = %q, want %q", enc.Name(), "libx265")
	}
}

// TestLibX264Encoder_IntegrationEncode runs a real ffmpeg encode if ffmpeg is
// available and libx264 is compiled in. The test generates a short synthetic
// YUV via ffmpeg's lavfi source so it does not depend on any external fixture.
func TestLibX264Encoder_IntegrationEncode(t *testing.T) {
	t.Parallel()

	if _, err := exec.LookPath("ffmpeg"); err != nil {
		t.Skip("ffmpeg not found on PATH — skipping integration encode test")
	}

	// Generate a 2-second 320×240 synthetic YUV source using ffmpeg.
	dir := t.TempDir()
	rawSrc := filepath.Join(dir, "src.y4m")
	genArgs := []string{
		"ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
		"-f", "lavfi",
		"-i", "testsrc=size=320x240:rate=24:duration=2",
		"-pix_fmt", "yuv420p",
		rawSrc,
	}
	if out, err := exec.Command(genArgs[0], genArgs[1:]...).CombinedOutput(); err != nil {
		t.Skipf("failed to generate synthetic source (%v): %s", err, out)
	}
	if _, statErr := os.Stat(rawSrc); os.IsNotExist(statErr) {
		t.Skip("generated synthetic source not found — skipping")
	}

	enc := encoder.LibX264Encoder{}
	params := encoder.EncodeParams{
		CRF:       28,
		OutputDir: dir,
	}
	result, err := enc.Encode(rawSrc, params)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	defer os.Remove(result.OutputPath)

	if result.OutputPath == "" {
		t.Error("OutputPath is empty")
	}
	if result.EncodeTimeMS < 0 {
		t.Errorf("EncodeTimeMS = %f, want >= 0", result.EncodeTimeMS)
	}
	// Bitrate may be 0 if ffprobe is not available — that is tolerated.
	t.Logf("encode result: CRF=%d, bitrate=%.1f kbps, time=%.1f ms, version=%q",
		params.CRF, result.BitratekBps, result.EncodeTimeMS, result.EncoderVersion)
}
