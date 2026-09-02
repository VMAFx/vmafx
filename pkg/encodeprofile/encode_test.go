// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/encodeprofile/encode_test.go — tests for the argv builder, the version
// parsers and the encode driver.
//
// testdata/pv_expected.json is the ParseVersions golden: 13 encoder-stderr
// samples x 13 encoder names = 169 cases, each run through the PYTHON
// vmaftune.encode.parse_versions and recorded verbatim.

package encodeprofile

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"testing"
)

// TestParseVersionsMatchesPython replays the full stderr x encoder matrix
// against the bytes CPython's parse_versions produced.
func TestParseVersionsMatchesPython(t *testing.T) {
	t.Parallel()

	b, err := os.ReadFile(filepath.Join("testdata", "pv_expected.json"))
	if err != nil {
		t.Fatalf("read golden: %v", err)
	}
	var golden struct {
		Stderrs map[string]string `json:"stderrs"`
		Cases   []struct {
			Stderr         string `json:"stderr"`
			Encoder        string `json:"encoder"`
			FFmpeg         string `json:"ffmpeg"`
			EncoderVersion string `json:"encoder_version"`
		} `json:"cases"`
	}
	if err := json.NewDecoder(bytes.NewReader(b)).Decode(&golden); err != nil {
		t.Fatalf("decode golden: %v", err)
	}
	if len(golden.Cases) == 0 {
		t.Fatal("golden has no cases")
	}

	for _, c := range golden.Cases {
		stderr, ok := golden.Stderrs[c.Stderr]
		if !ok {
			t.Fatalf("golden references unknown stderr sample %q", c.Stderr)
		}
		gotFF, gotEnc := ParseVersions(stderr, c.Encoder)
		if gotFF != c.FFmpeg || gotEnc != c.EncoderVersion {
			t.Errorf("ParseVersions(%s, %q) = (%q, %q), want (%q, %q)",
				c.Stderr, c.Encoder, gotFF, gotEnc, c.FFmpeg, c.EncoderVersion)
		}
	}
}

// TestParseVersionsBehaviour spells out the rules the golden matrix encodes,
// so a failure points at the intent rather than at a row index.
func TestParseVersionsBehaviour(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name        string
		stderr      string
		encoder     string
		wantFFmpeg  string
		wantEncoder string
	}{
		{
			name:       "nothing parseable yields unknown for both",
			stderr:     "",
			encoder:    "libx264",
			wantFFmpeg: "unknown", wantEncoder: "unknown",
		},
		{
			name:       "ffmpeg version is read independently of the encoder",
			stderr:     "ffmpeg version n8.1 Copyright (c) 2000-2025\n",
			encoder:    "libx265",
			wantFFmpeg: "n8.1", wantEncoder: "unknown",
		},
		{
			name:        "x264 canonical banner",
			stderr:      "ffmpeg version n8.1\nx264 - core 164 r3094 bfc87b7\n",
			encoder:     "libx264",
			wantFFmpeg:  "n8.1",
			wantEncoder: "libx264-164",
		},
		{
			// Some downstream builds print "x264-core 161" in the configure
			// summary instead of the canonical banner (ADR-0498 follow-up #7).
			name:        "x264 hyphenated banner variant",
			stderr:      "ffmpeg version 7.1\nx264-core 161\n",
			encoder:     "libx264",
			wantFFmpeg:  "7.1",
			wantEncoder: "libx264-161",
		},
		{
			// An empty encoder means "caller passed no override": auto-detect,
			// with x264 winning over x265 in a multi-codec log.
			name: "empty encoder auto-detects, x264 first",
			stderr: "ffmpeg version n8.1\nx264 - core 164 r3094\n" +
				"x265 [info]: HEVC encoder version 3.5\n",
			encoder:     "",
			wantFFmpeg:  "n8.1",
			wantEncoder: "libx264-164",
		},
		{
			name:        "x265 banner",
			stderr:      "ffmpeg version n8.1\nx265 [info]: HEVC encoder version 3.5+1-f0c1022b6\n",
			encoder:     "libx265",
			wantFFmpeg:  "n8.1",
			wantEncoder: "libx265-3.5+1-f0c1022b6",
		},
		{
			name:        "svt-av1 old banner",
			stderr:      "ffmpeg version n8.1\nSVT-AV1 ENCODER v1.7.0\n",
			encoder:     "libsvtav1",
			wantFFmpeg:  "n8.1",
			wantEncoder: "libsvtav1-1.7.0",
		},
		{
			name:        "svt-av1 new banner",
			stderr:      "ffmpeg version n8.1\nSvt[info]:SVT-AV1 Encoder Lib v2.1.0\n",
			encoder:     "libsvtav1",
			wantFFmpeg:  "n8.1",
			wantEncoder: "libsvtav1-2.1.0",
		},
		{
			name:        "libvpx-vp9 banner",
			stderr:      "ffmpeg version n8.1\n[libvpx-vp9 @ 0x5620] v1.13.1\n",
			encoder:     "libvpx-vp9",
			wantFFmpeg:  "n8.1",
			wantEncoder: "libvpx-vp9-1.13.1",
		},
		{
			name:        "libaom AOM-version banner variant",
			stderr:      "ffmpeg version n8.1\n[libaom @ 0x5620] AOM version: 3.8.0\n",
			encoder:     "libaom-av1",
			wantFFmpeg:  "n8.1",
			wantEncoder: "libaom-av1-3.8.0",
		},
		{
			// libaom and VVenC fall back to the stable adapter name rather
			// than "unknown" when the banner is absent.
			name:        "libaom falls back to the adapter name",
			stderr:      "ffmpeg version n8.1\n",
			encoder:     "libaom-av1",
			wantFFmpeg:  "n8.1",
			wantEncoder: "libaom-av1",
		},
		{
			name:        "vvenc long banner",
			stderr:      "ffmpeg version n8.1\n[libvvenc @ 0x5620] Fraunhofer VVC/H.266 Encoder VVenC v1.14.0\n",
			encoder:     "libvvenc",
			wantFFmpeg:  "n8.1",
			wantEncoder: "libvvenc-1.14.0",
		},
		{
			// Hardware encoders advertise no version, so the token itself is
			// the stable identifier.
			name:        "nvenc returns its own token",
			stderr:      "ffmpeg version n8.1\n",
			encoder:     "h264_nvenc",
			wantFFmpeg:  "n8.1",
			wantEncoder: "h264_nvenc",
		},
		{
			name:        "qsv returns its own token",
			stderr:      "",
			encoder:     "av1_qsv",
			wantFFmpeg:  "unknown",
			wantEncoder: "av1_qsv",
		},
		{
			name:        "a wholly unknown encoder is unknown",
			stderr:      "ffmpeg version n8.1\n",
			encoder:     "libtheora",
			wantFFmpeg:  "n8.1",
			wantEncoder: "unknown",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			gotFF, gotEnc := ParseVersions(tt.stderr, tt.encoder)
			if gotFF != tt.wantFFmpeg || gotEnc != tt.wantEncoder {
				t.Errorf("ParseVersions = (%q, %q), want (%q, %q)",
					gotFF, gotEnc, tt.wantFFmpeg, tt.wantEncoder)
			}
		})
	}
}

// TestBuildFFmpegCommandInputSideOptions pins where -ss / -t land. They must
// precede -i: output-side seeking would still decode the whole source, which
// is the entire point of sample-clip mode (ADR-0297).
func TestBuildFFmpegCommandInputSideOptions(t *testing.T) {
	t.Parallel()

	base := EncodeRequest{
		Source: "ref.yuv", Width: 320, Height: 240, PixFmt: "yuv420p",
		Framerate: 30, Encoder: "libx264", Preset: "medium", CRF: 23,
		Output: "out.mkv",
	}

	tests := []struct {
		name string
		mut  func(r *EncodeRequest)
		want []string
	}{
		{
			name: "raw source declares geometry",
			mut:  func(*EncodeRequest) {},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "320x240", "-r", "30.0",
				"-i", "ref.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "23",
				"out.mkv",
			},
		},
		{
			name: "container source omits the geometry flags",
			mut: func(r *EncodeRequest) {
				r.SourceIsContainer = true
				r.Source = "ref.mp4"
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-i", "ref.mp4",
				"-c:v", "libx264", "-preset", "medium", "-crf", "23",
				"out.mkv",
			},
		},
		{
			name: "sample clip inserts -ss and -t before -i",
			mut: func(r *EncodeRequest) {
				r.SampleClipSeconds = 2.5
				r.SampleClipStartS = 3.75
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "320x240", "-r", "30.0",
				"-ss", "3.75", "-t", "2.5",
				"-i", "ref.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "23",
				"out.mkv",
			},
		},
		{
			// ADR-0506: a bound duration still clips the encode when
			// sample-clip mode is off.
			name: "bound duration becomes an input-side -t",
			mut:  func(r *EncodeRequest) { r.DurationS = 10 },
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "320x240", "-r", "30.0",
				"-t", "10.0",
				"-i", "ref.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "23",
				"out.mkv",
			},
		},
		{
			name: "sample clip takes precedence over a bound duration",
			mut: func(r *EncodeRequest) {
				r.DurationS = 10
				r.SampleClipSeconds = 2
				r.SampleClipStartS = 4
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "320x240", "-r", "30.0",
				"-ss", "4.0", "-t", "2.0",
				"-i", "ref.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "23",
				"out.mkv",
			},
		},
		{
			name: "extra params land after the codec args",
			mut: func(r *EncodeRequest) {
				r.SourceIsContainer = true
				r.ExtraParams = []string{"-movflags", "+faststart"}
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-i", "ref.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "23",
				"-movflags", "+faststart",
				"out.mkv",
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			req := base
			tt.mut(&req)
			got, err := BuildFFmpegCommand(req, "ffmpeg")
			if err != nil {
				t.Fatalf("BuildFFmpegCommand: %v", err)
			}
			if !reflect.DeepEqual(got, tt.want) {
				t.Errorf("argv =\n %v\nwant\n %v", got, tt.want)
			}
		})
	}
}

// TestBuildFFmpegCommandFloatFormatting pins the float rendering in the argv.
// Python interpolates floats with str(), so 30.0 renders "30.0" — a Go
// %v/strconv default would emit "30" and silently change the command line.
func TestBuildFFmpegCommandFloatFormatting(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		framerate float64
		want      string
	}{
		{"integral framerate keeps its dot zero", 30, "30.0"},
		{"fractional framerate", 29.97, "29.97"},
		{"ntsc framerate", 23.976, "23.976"},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			req := EncodeRequest{
				Source: "ref.yuv", Width: 320, Height: 240, PixFmt: "yuv420p",
				Framerate: tt.framerate, Encoder: "libx264", Preset: "medium",
				CRF: 23, Output: "out.mkv",
			}
			argv, err := BuildFFmpegCommand(req, "ffmpeg")
			if err != nil {
				t.Fatalf("BuildFFmpegCommand: %v", err)
			}
			idx := slices.Index(argv, "-r")
			if idx < 0 || idx+1 >= len(argv) {
				t.Fatalf("argv has no -r flag: %v", argv)
			}
			if argv[idx+1] != tt.want {
				t.Errorf("-r value = %q, want %q", argv[idx+1], tt.want)
			}
		})
	}
}

func TestBuildFFmpegCommandDefaultsBinary(t *testing.T) {
	t.Parallel()

	req := EncodeRequest{
		Source: "ref.mp4", SourceIsContainer: true, Encoder: "libx264",
		Preset: "medium", CRF: 23, Output: "out.mkv",
	}
	argv, err := BuildFFmpegCommand(req, "")
	if err != nil {
		t.Fatalf("BuildFFmpegCommand: %v", err)
	}
	if argv[0] != "ffmpeg" {
		t.Errorf("argv[0] = %q, want %q", argv[0], "ffmpeg")
	}
}

func TestBuildFFmpegCommandRejectsBadPreset(t *testing.T) {
	t.Parallel()

	req := EncodeRequest{
		Source: "ref.mp4", SourceIsContainer: true, Encoder: "libx264",
		Preset: "placebo", // x265-only rung
		CRF:    23, Output: "out.mkv",
	}
	if _, err := BuildFFmpegCommand(req, "ffmpeg"); err == nil {
		t.Fatal("BuildFFmpegCommand accepted an out-of-vocabulary preset")
	}
}

// stubRunner returns a Runner that records the argvs it saw and replays canned
// results in order.
func stubRunner(seen *[][]string, results ...RunResult) Runner {
	i := 0
	return func(argv []string) (RunResult, error) {
		*seen = append(*seen, append([]string(nil), argv...))
		if i < len(results) {
			r := results[i]
			i++
			return r, nil
		}
		return RunResult{}, nil
	}
}

func TestRunEncode(t *testing.T) {
	t.Parallel()

	t.Run("successful encode records size and versions", func(t *testing.T) {
		t.Parallel()

		out := filepath.Join(t.TempDir(), "out.mkv")
		if err := os.WriteFile(out, bytes.Repeat([]byte{0}, 4096), 0o600); err != nil {
			t.Fatalf("seed output: %v", err)
		}

		req := EncodeRequest{
			Source: "ref.mp4", SourceIsContainer: true, Encoder: "libx264",
			Preset: "medium", CRF: 23, Output: out,
		}
		var seen [][]string
		run := stubRunner(&seen, RunResult{
			Stderr:     "ffmpeg version n8.1\nx264 - core 164 r3094 bfc87b7\n",
			ReturnCode: 0,
		})

		res, err := RunEncode(req, "ffmpeg", run)
		if err != nil {
			t.Fatalf("RunEncode: %v", err)
		}
		if res.ExitStatus != 0 {
			t.Errorf("ExitStatus = %d, want 0", res.ExitStatus)
		}
		if res.EncodeSizeBytes != 4096 {
			t.Errorf("EncodeSizeBytes = %d, want 4096", res.EncodeSizeBytes)
		}
		if res.FFmpegVersion != "n8.1" {
			t.Errorf("FFmpegVersion = %q, want %q", res.FFmpegVersion, "n8.1")
		}
		if res.EncoderVersion != "libx264-164" {
			t.Errorf("EncoderVersion = %q, want %q", res.EncoderVersion, "libx264-164")
		}
		if len(seen) != 1 {
			t.Fatalf("runner called %d times, want 1", len(seen))
		}
		if seen[0][0] != "ffmpeg" {
			t.Errorf("argv[0] = %q, want ffmpeg", seen[0][0])
		}
	})

	t.Run("failed encode reports the exit status and skips the size probe", func(t *testing.T) {
		t.Parallel()

		out := filepath.Join(t.TempDir(), "out.mkv")
		if err := os.WriteFile(out, []byte("partial"), 0o600); err != nil {
			t.Fatalf("seed output: %v", err)
		}

		req := EncodeRequest{
			Source: "ref.mp4", SourceIsContainer: true, Encoder: "libx264",
			Preset: "medium", CRF: 23, Output: out,
		}
		var seen [][]string
		run := stubRunner(&seen, RunResult{Stderr: "boom\n", ReturnCode: 234})

		res, err := RunEncode(req, "ffmpeg", run)
		if err != nil {
			t.Fatalf("RunEncode: %v", err)
		}
		if res.ExitStatus != 234 {
			t.Errorf("ExitStatus = %d, want 234", res.ExitStatus)
		}
		// A non-zero exit leaves the size at zero even though a partial file
		// exists on disk, matching Python.
		if res.EncodeSizeBytes != 0 {
			t.Errorf("EncodeSizeBytes = %d, want 0 on a failed encode", res.EncodeSizeBytes)
		}
		if res.StderrTail != "boom\n" {
			t.Errorf("StderrTail = %q, want %q", res.StderrTail, "boom\n")
		}
	})

	t.Run("stderr tail is capped", func(t *testing.T) {
		t.Parallel()

		long := strings.Repeat("x", stderrTailBytes+500) + "TAIL"
		req := EncodeRequest{
			Source: "ref.mp4", SourceIsContainer: true, Encoder: "h264_nvenc",
			Preset: "medium", CRF: 23, Output: filepath.Join(t.TempDir(), "o.mkv"),
		}
		var seen [][]string
		run := stubRunner(&seen, RunResult{Stderr: long, ReturnCode: 1})

		res, err := RunEncode(req, "ffmpeg", run)
		if err != nil {
			t.Fatalf("RunEncode: %v", err)
		}
		if len(res.StderrTail) != stderrTailBytes {
			t.Errorf("StderrTail length = %d, want %d", len(res.StderrTail), stderrTailBytes)
		}
		if !strings.HasSuffix(res.StderrTail, "TAIL") {
			t.Error("StderrTail kept the head instead of the tail")
		}
	})

	t.Run("runner failure surfaces as an error", func(t *testing.T) {
		t.Parallel()

		wantErr := errors.New("exec format error")
		req := EncodeRequest{
			Source: "ref.mp4", SourceIsContainer: true, Encoder: "libx264",
			Preset: "medium", CRF: 23, Output: "out.mkv",
		}
		run := func([]string) (RunResult, error) { return RunResult{}, wantErr }

		if _, err := RunEncode(req, "ffmpeg", run); !errors.Is(err, wantErr) {
			t.Errorf("RunEncode err = %v, want %v", err, wantErr)
		}
	})

	t.Run("unbuildable argv fails before any subprocess", func(t *testing.T) {
		t.Parallel()

		req := EncodeRequest{
			Source: "ref.mp4", SourceIsContainer: true, Encoder: "libx264",
			Preset: "nonsense", CRF: 23, Output: "out.mkv",
		}
		called := false
		run := func([]string) (RunResult, error) { called = true; return RunResult{}, nil }

		if _, err := RunEncode(req, "ffmpeg", run); err == nil {
			t.Fatal("RunEncode succeeded with an unbuildable argv")
		}
		if called {
			t.Error("RunEncode spawned a subprocess despite the argv failing to build")
		}
	})
}

// TestRunEncodeProbesVersionFallback covers the ADR-0498 follow-up #7 path:
// modern FFmpeg builds suppress the per-encoder banner under -hide_banner, so
// a successful encode with an unparseable version re-probes `ffmpeg -version`
// and settles for an "<encoder>-enabled" marker.
func TestRunEncodeProbesVersionFallback(t *testing.T) {
	// Not parallel: it mutates the package-level probe cache.
	probeCache.Clear()
	t.Cleanup(func() { probeCache.Clear() })

	out := filepath.Join(t.TempDir(), "out.mkv")
	if err := os.WriteFile(out, []byte("x"), 0o600); err != nil {
		t.Fatalf("seed output: %v", err)
	}
	req := EncodeRequest{
		Source: "ref.mp4", SourceIsContainer: true, Encoder: "libx264",
		Preset: "medium", CRF: 23, Output: out,
	}

	var seen [][]string
	run := stubRunner(&seen,
		// The encode itself: succeeds, but the banner is suppressed.
		RunResult{Stderr: "ffmpeg version n8.1\n", ReturnCode: 0},
		// The fallback `ffmpeg -version` probe.
		RunResult{Stdout: "configuration: --enable-libx264 --enable-libx265\n"},
	)

	res, err := RunEncode(req, "/opt/ffmpeg/bin/ffmpeg", run)
	if err != nil {
		t.Fatalf("RunEncode: %v", err)
	}
	if res.EncoderVersion != "libx264-enabled" {
		t.Errorf("EncoderVersion = %q, want %q", res.EncoderVersion, "libx264-enabled")
	}
	if len(seen) != 2 {
		t.Fatalf("runner called %d times, want 2 (encode + version probe)", len(seen))
	}
	wantProbe := []string{"/opt/ffmpeg/bin/ffmpeg", "-version"}
	if !reflect.DeepEqual(seen[1], wantProbe) {
		t.Errorf("probe argv = %v, want %v", seen[1], wantProbe)
	}
}

func TestProbeEncoderVersion(t *testing.T) {
	probeCache.Clear()
	t.Cleanup(func() { probeCache.Clear() })

	tests := []struct {
		name    string
		encoder string
		stdout  string
		want    string
	}{
		{
			name: "configure flag present", encoder: "libx265",
			stdout: "configuration: --enable-libx265\n", want: "libx265-enabled",
		},
		{
			name: "configure flag absent", encoder: "libvvenc",
			stdout: "configuration: --enable-gpl\n", want: "",
		},
		{
			// An encoder with no probe pattern never shells out at all.
			name: "unprobeable encoder", encoder: "h264_nvenc",
			stdout: "configuration: --enable-nvenc\n", want: "",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			var seen [][]string
			run := stubRunner(&seen, RunResult{Stdout: tt.stdout})
			// A distinct binary path per subtest keeps the cache keys apart.
			got := probeEncoderVersion("ffmpeg-"+tt.name, tt.encoder, run)
			if got != tt.want {
				t.Errorf("probeEncoderVersion = %q, want %q", got, tt.want)
			}
		})
	}
}

func TestProbeEncoderVersionIsCached(t *testing.T) {
	probeCache.Clear()
	t.Cleanup(func() { probeCache.Clear() })

	var seen [][]string
	run := stubRunner(&seen,
		RunResult{Stdout: "configuration: --enable-libx264\n"},
		RunResult{Stdout: "configuration: --enable-libx264\n"},
	)

	first := probeEncoderVersion("ffmpeg-cached", "libx264", run)
	second := probeEncoderVersion("ffmpeg-cached", "libx264", run)
	if first != second || first != "libx264-enabled" {
		t.Errorf("probe results = (%q, %q), want both %q", first, second, "libx264-enabled")
	}
	if len(seen) != 1 {
		t.Errorf("probe ran %d times, want 1 (result should be cached)", len(seen))
	}
}
