// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/encoder/version_extract_test.go — table-driven unit tests for the
// extractEncoderVersion helper and the ffmpegBin/outputDir defaulting
// helpers in encoder.go. These functions are not covered by the existing
// discover_test.go (PR #347) because they operate purely on strings, not
// the ffmpeg/ffprobe subprocesses that discover_test.go targets.

package encoder

import (
	"os"
	"strings"
	"testing"
)

func TestExtractEncoderVersion_LibX264TypicalBanner(t *testing.T) {
	t.Parallel()
	stderr := strings.Join([]string{
		"ffmpeg version 6.1 Copyright (c) 2000-2024 the FFmpeg developers",
		"  libavutil      58. 29.100 / 58. 29.100",
		"[libx264 @ 0x55d1] using SAR=1/1",
		"libx264 - core 164 r3094M 0bfe3e0 - H.264/MPEG-4 AVC codec - Copyleft 2003-2024",
		"Output #0, mp4, to '/tmp/out.mp4':",
	}, "\n")

	got := extractEncoderVersion(stderr, "libx264")
	if got == "" {
		t.Fatalf("extractEncoderVersion: empty result")
	}
	if !strings.Contains(got, "core 164") {
		t.Errorf("expected 'core 164' in version, got %q", got)
	}
}

func TestExtractEncoderVersion_NoBannerLine(t *testing.T) {
	t.Parallel()
	// No line containing both the codec and "core" → empty result.
	stderr := "ffmpeg version 6.1\n[libx264 @ 0x1] some other line\n"
	got := extractEncoderVersion(stderr, "libx264")
	if got != "" {
		t.Errorf("expected empty when no banner line, got %q", got)
	}
}

func TestExtractEncoderVersion_NoDashSeparator(t *testing.T) {
	t.Parallel()
	// Line contains "libx265" and "core" but no dash → returns the full
	// trimmed line.
	stderr := "libx265 core 3.5 something"
	got := extractEncoderVersion(stderr, "libx265")
	if got == "" {
		t.Fatalf("expected non-empty fallback")
	}
	if !strings.Contains(got, "libx265") {
		t.Errorf("expected libx265 in fallback, got %q", got)
	}
}

func TestExtractEncoderVersion_NotPresent(t *testing.T) {
	t.Parallel()
	got := extractEncoderVersion("nothing matches here", "libx264")
	if got != "" {
		t.Errorf("expected empty for missing codec, got %q", got)
	}
}

func TestExtractEncoderVersion_MultiLineFindsFirst(t *testing.T) {
	t.Parallel()
	stderr := strings.Join([]string{
		"libx264 - core 161 r3018 - older line",
		"libx264 - core 164 r3094 - newer line",
	}, "\n")
	got := extractEncoderVersion(stderr, "libx264")
	if !strings.Contains(got, "core 161") {
		t.Errorf("expected first match, got %q", got)
	}
}

// ---------------------------------------------------------------------------
// ffmpegBin defaulting
// ---------------------------------------------------------------------------

func TestFFmpegBin_DefaultsToFFmpeg(t *testing.T) {
	t.Parallel()
	got := ffmpegBin(EncodeParams{})
	if got != "ffmpeg" {
		t.Errorf("ffmpegBin({}) = %q, want ffmpeg", got)
	}
}

func TestFFmpegBin_RespectsExplicitPath(t *testing.T) {
	t.Parallel()
	got := ffmpegBin(EncodeParams{FFmpegBin: "/opt/ffmpeg-6.1/bin/ffmpeg"})
	if got != "/opt/ffmpeg-6.1/bin/ffmpeg" {
		t.Errorf("ffmpegBin: got %q", got)
	}
}

// ---------------------------------------------------------------------------
// outputDir defaulting
// ---------------------------------------------------------------------------

func TestOutputDir_DefaultsToTempDir(t *testing.T) {
	t.Parallel()
	got := outputDir(EncodeParams{})
	if got != os.TempDir() {
		t.Errorf("outputDir({}) = %q, want os.TempDir() = %q", got, os.TempDir())
	}
}

func TestOutputDir_RespectsExplicitPath(t *testing.T) {
	t.Parallel()
	got := outputDir(EncodeParams{OutputDir: "/var/tmp/vmaf"})
	if got != "/var/tmp/vmaf" {
		t.Errorf("outputDir: got %q, want /var/tmp/vmaf", got)
	}
}
