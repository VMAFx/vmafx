// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package hdr

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"testing"
)

// quietLogger silences the "treating as SDR" / "no HDR dispatch" warnings the
// detection and dispatch paths emit by design.
func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, &slog.HandlerOptions{Level: slog.LevelError}))
}

// pythonHDRFixture mirrors testdata/python_hdr.json, dumped from vmaftune.hdr.
type pythonHDRFixture struct {
	Classify map[string]struct {
		Payload json.RawMessage `json:"payload"`
		Info    *struct {
			Transfer      string  `json:"transfer"`
			Primaries     string  `json:"primaries"`
			Matrix        string  `json:"matrix"`
			ColorRange    string  `json:"color_range"`
			PixFmt        string  `json:"pix_fmt"`
			MasterDisplay *string `json:"master_display"`
			MaxCLL        *string `json:"max_cll"`
		} `json:"info"`
	} `json:"classify"`
	CodecArgs map[string]map[string][]string `json:"codec_args"`
}

func loadHDRFixture(t *testing.T) pythonHDRFixture {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join("testdata", "python_hdr.json"))
	if err != nil {
		t.Fatalf("read hdr fixture: %v", err)
	}
	var fixture pythonHDRFixture
	if err := json.Unmarshal(raw, &fixture); err != nil {
		t.Fatalf("parse hdr fixture: %v", err)
	}
	if len(fixture.Classify) == 0 {
		t.Fatal("hdr fixture is empty")
	}
	return fixture
}

// TestClassifyPayloadMatchesPython replays the ffprobe payloads through both
// classifiers. The permissive-detection contract is the point: a PQ transfer
// with non-BT.2020 primaries must come back SDR on both sides, because
// misclassifying SDR as HDR would inject PQ signalling into a gamma-2.4
// encode.
func TestClassifyPayloadMatchesPython(t *testing.T) {
	t.Parallel()

	fixture := loadHDRFixture(t)
	for name, tc := range fixture.Classify {
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			got := ClassifyPayload(tc.Payload, quietLogger())

			if tc.Info == nil {
				if got != nil {
					t.Fatalf("expected SDR (nil), got %+v", got)
				}
				return
			}
			if got == nil {
				t.Fatalf("expected HDR info, got nil")
			}
			want := Info{
				Transfer:   tc.Info.Transfer,
				Primaries:  tc.Info.Primaries,
				Matrix:     tc.Info.Matrix,
				ColorRange: tc.Info.ColorRange,
				PixFmt:     tc.Info.PixFmt,
			}
			if tc.Info.MasterDisplay != nil {
				want.MasterDisplay = *tc.Info.MasterDisplay
			}
			if tc.Info.MaxCLL != nil {
				want.MaxCLL = *tc.Info.MaxCLL
			}
			if !reflect.DeepEqual(*got, want) {
				t.Errorf("classified as\n %+v\nwant\n %+v", *got, want)
			}
		})
	}
}

// TestCodecArgsMatchPython replays the dispatch table for every classified
// payload against every encoder the Python table covers, plus two it does not
// (h264_nvenc, an unknown codec) which must produce an empty argv.
func TestCodecArgsMatchPython(t *testing.T) {
	t.Parallel()

	fixture := loadHDRFixture(t)
	for payloadName, perEncoder := range fixture.CodecArgs {
		entry := fixture.Classify[payloadName]
		if entry.Info == nil {
			t.Fatalf("fixture %q has codec args but classified as SDR", payloadName)
		}
		info := ClassifyPayload(entry.Payload, quietLogger())
		if info == nil {
			t.Fatalf("fixture %q: Go classified it as SDR", payloadName)
		}

		encoders := make([]string, 0, len(perEncoder))
		for enc := range perEncoder {
			encoders = append(encoders, enc)
		}
		sort.Strings(encoders)

		for _, encoder := range encoders {
			t.Run(payloadName+"/"+encoder, func(t *testing.T) {
				t.Parallel()
				want := perEncoder[encoder]
				got := CodecArgs(encoder, info, quietLogger())
				if len(want) == 0 && len(got) == 0 {
					return
				}
				if !reflect.DeepEqual(got, want) {
					t.Errorf("args =\n %v\nwant\n %v", got, want)
				}
			})
		}
	}
}

// TestCodecArgsNilInfo pins the SDR path: a nil Info yields no flags for any
// encoder, so the caller can pass the detection result straight through.
func TestCodecArgsNilInfo(t *testing.T) {
	t.Parallel()

	for _, encoder := range []string{"libx265", "libsvtav1", "hevc_nvenc", "unknown"} {
		if got := CodecArgs(encoder, nil, quietLogger()); len(got) != 0 {
			t.Errorf("CodecArgs(%q, nil) = %v, want empty", encoder, got)
		}
	}
}

// TestDefaultForMetadataOnly pins the conservative BT.2020/PQ tuple used when
// the caller knows a source is HDR but has no probed payload.
func TestDefaultForMetadataOnly(t *testing.T) {
	t.Parallel()

	got := DefaultForMetadataOnly()
	want := &Info{
		Transfer:   "pq",
		Primaries:  "bt2020",
		Matrix:     "bt2020nc",
		ColorRange: "tv",
		PixFmt:     "yuv420p10le",
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("DefaultForMetadataOnly = %+v, want %+v", got, want)
	}
}

// TestDetectDegradesToSDR pins every failure mode of the probe: a spawn
// error, a non-zero exit, and unparseable output all mean "treat as SDR"
// rather than an error the caller has to handle.
func TestDetectDegradesToSDR(t *testing.T) {
	t.Parallel()

	pq := `{"streams":[{"color_transfer":"smpte2084","color_primaries":"bt2020",` +
		`"color_space":"bt2020nc","color_range":"tv","pix_fmt":"yuv420p10le"}]}`

	tests := []struct {
		name    string
		stdout  string
		code    int
		err     error
		wantHDR bool
	}{
		{"clean PQ probe", pq, 0, nil, true},
		{"ffprobe not installed", "", 0, errors.New("exec: not found"), false},
		{"ffprobe exits non-zero", "", 1, nil, false},
		{"unparseable output", "not json", 0, nil, false},
		{"empty output", "", 0, nil, false},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			runner := func(_ context.Context, _ []string) (string, int, error) {
				return tc.stdout, tc.code, tc.err
			}
			got := Detect(context.Background(), "src.mkv", "ffprobe", runner, quietLogger())
			if (got != nil) != tc.wantHDR {
				t.Errorf("Detect returned %v, want HDR=%v", got, tc.wantHDR)
			}
		})
	}
}

// TestDetectPassesTheExpectedArgv pins the ffprobe invocation, which is the
// contract the -show_entries selector encodes: dropping a field here silently
// blanks the mastering-display SEI.
func TestDetectPassesTheExpectedArgv(t *testing.T) {
	t.Parallel()

	var seen []string
	runner := func(_ context.Context, argv []string) (string, int, error) {
		seen = argv
		return "", 1, nil
	}
	Detect(context.Background(), "clip.mkv", "/usr/bin/ffprobe", runner, quietLogger())

	if len(seen) == 0 {
		t.Fatal("runner was not invoked")
	}
	if seen[0] != "/usr/bin/ffprobe" {
		t.Errorf("argv[0] = %q, want the configured binary", seen[0])
	}
	if seen[len(seen)-1] != "clip.mkv" {
		t.Errorf("argv should end with the source path, got %q", seen[len(seen)-1])
	}
	joined := ""
	for _, a := range seen {
		joined += a + " "
	}
	for _, needle := range []string{
		"-select_streams", "v:0", "-show_streams", "-of", "json",
		"color_transfer", "side_data_type", "max_luminance", "max_content",
	} {
		if !containsSubstring(joined, needle) {
			t.Errorf("argv is missing %q: %v", needle, seen)
		}
	}
}

// TestFracToUnit pins the ffprobe fraction parsing the mastering-display SEI
// depends on.
func TestFracToUnit(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name  string
		value any
		scale int64
		want  int64
	}{
		{"fraction string", "34000/50000", 50000, 34000},
		{"fraction with whitespace", " 1 / 4 ", 50000, 12500},
		{"decimal string", "0.68", 50000, 34000},
		{"float", 0.68, 50000, 34000},
		{"int", 1, 10000, 10000},
		{"zero denominator", "1/0", 50000, 0},
		{"garbage", "not a number", 50000, 0},
		{"unsupported type", []int{1}, 50000, 0},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := fracToUnit(tc.value, tc.scale); got != tc.want {
				t.Errorf("fracToUnit(%v, %d) = %d, want %d", tc.value, tc.scale, got, tc.want)
			}
		})
	}
}

func containsSubstring(haystack, needle string) bool {
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if haystack[i:i+len(needle)] == needle {
			return true
		}
	}
	return false
}
