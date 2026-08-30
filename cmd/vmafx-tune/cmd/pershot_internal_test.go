// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// In-package unit tests for the tune-per-shot subcommand's pure helpers:
// CRF-range parsing, NEG model routing, raw-source detection, geometry
// resolution, the unported-flag guard, and the scratch-directory resolver.

package cmd

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseOptionalCRFRange(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name           string
		crfMin, crfMax int
		wantNil        bool
		want           [2]int
		wantErr        bool
	}{
		{
			// Neither supplied: the bisect falls back to the codec's
			// absolute quality window (ADR-0538).
			name: "neither supplied", crfMin: -1, crfMax: -1, wantNil: true,
		},
		{name: "both supplied", crfMin: 18, crfMax: 35, want: [2]int{18, 35}},
		{name: "equal bounds", crfMin: 23, crfMax: 23, want: [2]int{23, 23}},
		{name: "only min", crfMin: 18, crfMax: -1, wantErr: true},
		{name: "only max", crfMin: -1, crfMax: 35, wantErr: true},
		{name: "inverted", crfMin: 40, crfMax: 20, wantErr: true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := parseOptionalCRFRange(tc.crfMin, tc.crfMax)
			if tc.wantErr {
				if err == nil {
					t.Fatalf("parseOptionalCRFRange(%d, %d) = %v, want error",
						tc.crfMin, tc.crfMax, got)
				}
				return
			}
			if err != nil {
				t.Fatalf("parseOptionalCRFRange(%d, %d): %v", tc.crfMin, tc.crfMax, err)
			}
			if tc.wantNil {
				if got != nil {
					t.Errorf("expected nil range, got %v", *got)
				}
				return
			}
			if got == nil || *got != tc.want {
				t.Errorf("range = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestResolveVMAFModel(t *testing.T) {
	t.Parallel()
	cases := []struct {
		model string
		neg   bool
		want  string
	}{
		{"vmaf_v0.6.1", false, "vmaf_v0.6.1"},
		{"vmaf_v0.6.1", true, "vmaf_v0.6.1neg"},
		{"vmaf_4k_v0.6.1", true, "vmaf_4k_v0.6.1neg"},
		// Idempotent: an already-NEG name is untouched.
		{"vmaf_v0.6.1neg", true, "vmaf_v0.6.1neg"},
		// A path/version override is not a version identifier.
		{"path=/abs/model.json", true, "path=/abs/model.json"},
		{"version=custom", true, "version=custom"},
		// An unknown model gets the suffix so libvmaf reports a clear
		// missing-model error rather than silently using the wrong one.
		{"my_model", true, "my_modelneg"},
	}
	for _, tc := range cases {
		if got := resolveVMAFModel(tc.model, tc.neg); got != tc.want {
			t.Errorf("resolveVMAFModel(%q, %v) = %q, want %q",
				tc.model, tc.neg, got, tc.want)
		}
	}
}

func TestSourceNeedsRawvideoDemux(t *testing.T) {
	t.Parallel()
	cases := map[string]bool{
		"clip.yuv":  true,
		"clip.YUV":  true,
		"clip.raw":  true,
		"clip.mp4":  false,
		"clip.mkv":  false,
		"clip.y4m":  false,
		"clip":      false,
		"a.yuv.mp4": false,
	}
	for path, want := range cases {
		if got := sourceNeedsRawvideoDemux(path); got != want {
			t.Errorf("sourceNeedsRawvideoDemux(%q) = %v, want %v", path, got, want)
		}
	}
}

func TestResolvePerShotGeometry(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name    string
		flags   perShotFlags
		want    perShotGeometry
		wantErr string
	}{
		{
			name: "raw YUV with explicit geometry",
			flags: perShotFlags{
				src: "clip.yuv", width: 320, height: 240,
				framerate: 25, totalFrames: 100,
			},
			want: perShotGeometry{width: 320, height: 240, framerate: 25, totalFrames: 100},
		},
		{
			// Raw YUV carries no header, so a missing framerate falls back
			// to the documented 24.0 default rather than probing.
			name:  "raw YUV without framerate defaults to 24",
			flags: perShotFlags{src: "clip.yuv", width: 320, height: 240},
			want:  perShotGeometry{width: 320, height: 240, framerate: 24},
		},
		{
			name:    "raw YUV without geometry is rejected",
			flags:   perShotFlags{src: "clip.yuv"},
			wantErr: "--width and --height are required for raw YUV",
		},
		{
			name:    "raw YUV with only width is rejected",
			flags:   perShotFlags{src: "clip.yuv", width: 320},
			wantErr: "--width and --height are required for raw YUV",
		},
		{
			// A container whose probe fails (the file does not exist) and
			// which was given no explicit geometry cannot proceed.
			name:    "unprobeable container without geometry is rejected",
			flags:   perShotFlags{src: "/nonexistent/clip.mp4", ffmpegBin: "ffmpeg"},
			wantErr: "could not determine source width/height",
		},
		{
			// Explicit geometry on a container short-circuits the probe.
			name: "container with explicit geometry skips the probe",
			flags: perShotFlags{
				src: "/nonexistent/clip.mp4", width: 1920, height: 1080, framerate: 30,
			},
			want: perShotGeometry{width: 1920, height: 1080, framerate: 30},
		},
		{
			name: "negative total frames normalises to zero",
			flags: perShotFlags{
				src: "clip.yuv", width: 64, height: 64, framerate: 24, totalFrames: -5,
			},
			want: perShotGeometry{width: 64, height: 64, framerate: 24, totalFrames: 0},
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			flags := tc.flags
			got, err := resolvePerShotGeometry(&flags)
			if tc.wantErr != "" {
				if err == nil {
					t.Fatalf("resolvePerShotGeometry = %+v, want error %q", got, tc.wantErr)
				}
				if !strings.Contains(err.Error(), tc.wantErr) {
					t.Errorf("error = %v, want it to contain %q", err, tc.wantErr)
				}
				return
			}
			if err != nil {
				t.Fatalf("resolvePerShotGeometry: %v", err)
			}
			if got != tc.want {
				t.Errorf("geometry = %+v, want %+v", got, tc.want)
			}
		})
	}
}

func TestRejectUnportedPerShotFlags(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name    string
		flags   perShotFlags
		wantErr string
	}{
		{name: "clean", flags: perShotFlags{}},
		{
			name:    "predicate module names the Python fallback",
			flags:   perShotFlags{predicateModule: "mymod:pick"},
			wantErr: "vmaf-tune tune-per-shot --predicate-module mymod:pick",
		},
		{
			name:    "fast-nr names onnxruntime and the Python fallback",
			flags:   perShotFlags{fastNR: true},
			wantErr: "onnxruntime",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			err := rejectUnportedPerShotFlags(&tc.flags)
			if tc.wantErr == "" {
				if err != nil {
					t.Fatalf("unexpected error: %v", err)
				}
				return
			}
			if err == nil {
				t.Fatal("expected an error naming the Python fallback")
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("error = %v, want it to contain %q", err, tc.wantErr)
			}
		})
	}
}

// TestPerShotWorkdirParent is not parallel: three of its subtests mutate
// VMAFTUNE_WORKDIR via t.Setenv, which the testing package forbids combining
// with t.Parallel.
func TestPerShotWorkdirParent(t *testing.T) {
	t.Run("explicit workdir wins and is created", func(t *testing.T) {
		dir := filepath.Join(t.TempDir(), "nested", "work")
		if got := perShotWorkdirParent(dir); got != dir {
			t.Errorf("perShotWorkdirParent = %q, want %q", got, dir)
		}
		if _, err := os.Stat(dir); err != nil {
			t.Errorf("explicit workdir was not created: %v", err)
		}
	})

	t.Run("writable env var is honoured", func(t *testing.T) {
		dir := t.TempDir()
		t.Setenv("VMAFTUNE_WORKDIR", dir)
		if got := perShotWorkdirParent(""); got != dir {
			t.Errorf("perShotWorkdirParent = %q, want %q", got, dir)
		}
	})

	t.Run("unset env var falls back to the OS temp default", func(t *testing.T) {
		t.Setenv("VMAFTUNE_WORKDIR", "")
		if got := perShotWorkdirParent(""); got != "" {
			t.Errorf("perShotWorkdirParent = %q, want the empty OS-default marker", got)
		}
	})

	t.Run("unwritable env var falls back", func(t *testing.T) {
		// A path under a regular file can never be created, so this
		// exercises the "env var set but unusable" branch without needing
		// root or a read-only mount.
		f := filepath.Join(t.TempDir(), "not-a-dir")
		if err := os.WriteFile(f, []byte("x"), 0o600); err != nil {
			t.Fatalf("write fixture: %v", err)
		}
		t.Setenv("VMAFTUNE_WORKDIR", filepath.Join(f, "sub"))
		if got := perShotWorkdirParent(""); got != "" {
			t.Errorf("perShotWorkdirParent = %q, want the empty OS-default marker", got)
		}
	})
}

func TestLastLine(t *testing.T) {
	t.Parallel()
	cases := map[string]string{
		"a\nb\nreal error":         "real error",
		"only one line":            "only one line",
		"trailing blank\nlast\n\n": "last",
		"":                         "no stderr",
		"   \n  \n":                "no stderr",
	}
	for in, want := range cases {
		if got := lastLine(in); got != want {
			t.Errorf("lastLine(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestTrimFloat(t *testing.T) {
	t.Parallel()
	cases := map[float64]string{
		24:     "24",
		23.976: "23.976",
		29.97:  "29.97",
		60:     "60",
	}
	for in, want := range cases {
		if got := trimFloat(in); got != want {
			t.Errorf("trimFloat(%v) = %q, want %q", in, got, want)
		}
	}
}
