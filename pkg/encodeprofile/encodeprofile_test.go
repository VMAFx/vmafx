// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/encodeprofile/encodeprofile_test.go — parity tests for the Go port of
// vmaftune.encoder_profile and the single-pass slice of vmaftune.encode.
//
// testdata/ holds one synthetic report profile in each of the four shapes the
// loader accepts (report JSON, bare profile JSON, report Markdown, report
// HTML) plus a container-source and a legacy string-source variant.
// testdata/ep_expected.json is the golden: it was produced by driving the
// PYTHON implementation over the same fixtures —
//
//	profile = load_profile_payload(path)
//	rec     = select_recommendation(profile, **select_kwargs)
//	req     = build_encode_request(profile, rec, output=..., **build_kwargs)
//	argv    = build_ffmpeg_command(req, ffmpeg_bin=...)
//
// so TestSelectAndBuildMatchPython compares against the implementation this
// port replaces, not against the port's own output.

package encodeprofile

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func f64p(v float64) *float64 { return &v }
func intp(v int) *int         { return &v }

// goldenCase is one row of testdata/ep_expected.json.
type goldenCase struct {
	Selected map[string]any `json:"selected"`
	Argv     []string       `json:"argv"`
	Err      *string        `json:"err"`
}

func loadGolden(t *testing.T) map[string]goldenCase {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("testdata", "ep_expected.json"))
	if err != nil {
		t.Fatalf("read golden: %v", err)
	}
	dec := json.NewDecoder(bytes.NewReader(b))
	dec.UseNumber()
	var out map[string]goldenCase
	if err := dec.Decode(&out); err != nil {
		t.Fatalf("decode golden: %v", err)
	}
	return out
}

func td(name string) string { return filepath.Join("testdata", name) }

// TestSelectAndBuildMatchPython is the headline parity gate: for each case it
// loads a profile, selects a recommendation, builds the request and composes
// the ffmpeg argv, then compares both the selected row and the full argv with
// what CPython produced for identical inputs.
func TestSelectAndBuildMatchPython(t *testing.T) {
	t.Parallel()

	golden := loadGolden(t)

	tests := []struct {
		name  string
		file  string
		sel   SelectOptions
		build BuildOptions
		ffbin string
	}{
		// No filters: the first Pareto row with the lowest bitrate wins.
		{name: "default", file: "profile.json", ffbin: "ffmpeg"},
		// Codec / target filters narrow the candidate set.
		{name: "codec_x264", file: "profile.json", sel: SelectOptions{Codec: "libx264"}, ffbin: "ffmpeg"},
		{name: "target_90", file: "profile.json", sel: SelectOptions{TargetVMAF: f64p(90)}, ffbin: "ffmpeg"},
		{
			name: "codec_x264_target_90", file: "profile.json",
			sel:   SelectOptions{Codec: "libx264", TargetVMAF: f64p(90)},
			ffbin: "ffmpeg",
		},
		// The index applies AFTER filtering and AFTER the Pareto sort.
		{name: "index_0", file: "profile.json", sel: SelectOptions{Index: intp(0)}, ffbin: "ffmpeg"},
		{name: "index_3", file: "profile.json", sel: SelectOptions{Index: intp(3)}, ffbin: "ffmpeg"},
		// index 6 is the last row, reached only after the codec tie-break
		// orders the two non-Pareto rows that share a bitrate.
		{name: "index_6", file: "profile.json", sel: SelectOptions{Index: intp(6)}, ffbin: "ffmpeg"},

		// The four accepted container shapes all yield the same argv.
		{name: "bare_profile", file: "profile_bare.json", ffbin: "ffmpeg"},
		{name: "markdown", file: "profile.md", ffbin: "ffmpeg"},
		{name: "html", file: "profile.html", ffbin: "ffmpeg"},

		// A container source drops the raw-video input flags.
		{name: "container", file: "profile_container.json", ffbin: "ffmpeg"},
		// A profile whose "source" is a bare string, not a metadata dict.
		{name: "legacy_source", file: "profile_legacy_source.json", ffbin: "ffmpeg"},
		// A hardware codec, with an absolute ffmpeg path.
		{
			name: "nvenc", file: "profile.json",
			sel: SelectOptions{Codec: "h264_nvenc"}, ffbin: "/opt/ffmpeg/bin/ffmpeg",
		},

		// Sample-clip mode replaces the duration-derived -t with -ss/-t.
		{
			name: "sample_clip", file: "profile.json",
			build: BuildOptions{SampleClipSeconds: 2.5, SampleClipStartS: 3.75},
			ffbin: "ffmpeg",
		},
		// Every override at once, including a source path that needs
		// normalising.
		{
			name: "overrides", file: "profile.json", sel: SelectOptions{Codec: "libsvtav1"},
			build: BuildOptions{
				SourceOverride: "other//clip.yuv", PresetOverride: "fast",
				PixFmtOverride: "yuv420p10le", FramerateOverride: f64p(24),
				WidthOverride: intp(1280), HeightOverride: intp(720),
				DurationOverride: f64p(4),
			},
			ffbin: "ffmpeg",
		},
		// --source-kind overrides the extension-based guess in both
		// directions.
		{
			name: "source_kind_raw", file: "profile_container.json",
			build: BuildOptions{SourceKind: "raw"}, ffbin: "ffmpeg",
		},
		{
			name: "source_kind_container", file: "profile.json",
			build: BuildOptions{SourceKind: "container"}, ffbin: "ffmpeg",
		},
		// Raw --extra-ffmpeg-arg tokens land after the codec args.
		{
			name: "extra_params", file: "profile.json",
			build: BuildOptions{ExtraParams: []string{"-movflags", "+faststart"}},
			ffbin: "ffmpeg",
		},
		// An explicit zero duration suppresses the -t the profile would
		// otherwise contribute.
		{
			name: "duration_zero", file: "profile.json",
			build: BuildOptions{DurationOverride: f64p(0)}, ffbin: "ffmpeg",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()

			want, ok := golden[tt.name]
			if !ok {
				t.Fatalf("no golden entry for %q", tt.name)
			}
			if want.Err != nil {
				t.Fatalf("golden entry %q records a Python error: %s", tt.name, *want.Err)
			}

			profile, err := LoadProfilePayload(td(tt.file))
			if err != nil {
				t.Fatalf("LoadProfilePayload: %v", err)
			}
			rec, err := SelectRecommendation(profile, tt.sel)
			if err != nil {
				t.Fatalf("SelectRecommendation: %v", err)
			}
			if !sameJSON(rec, want.Selected) {
				t.Errorf("selected row differs from CPython\n got=%v\nwant=%v",
					rec, want.Selected)
			}

			build := tt.build
			build.Output = "out//encoded.mkv"
			req, err := BuildEncodeRequest(profile, rec, build)
			if err != nil {
				t.Fatalf("BuildEncodeRequest: %v", err)
			}
			argv, err := BuildFFmpegCommand(req, tt.ffbin)
			if err != nil {
				t.Fatalf("BuildFFmpegCommand: %v", err)
			}
			if !reflect.DeepEqual(argv, want.Argv) {
				t.Errorf("argv differs from CPython\n got=%v\nwant=%v", argv, want.Argv)
			}
		})
	}
}

// sameJSON compares two decoded JSON trees, collapsing json.Number so an int
// literal on one side matches the same value on the other.
func sameJSON(a, b map[string]any) bool {
	ja, _ := json.Marshal(normaliseJSON(a))
	jb, _ := json.Marshal(normaliseJSON(b))
	return string(ja) == string(jb)
}

func normaliseJSON(v any) any {
	switch t := v.(type) {
	case map[string]any:
		out := make(map[string]any, len(t))
		for k, val := range t {
			out[k] = normaliseJSON(val)
		}
		return out
	case []any:
		out := make([]any, len(t))
		for i, val := range t {
			out[i] = normaliseJSON(val)
		}
		return out
	case json.Number:
		if f, err := t.Float64(); err == nil {
			return f
		}
		return t.String()
	default:
		return v
	}
}

// TestSelectRecommendationErrors pins the rejection paths. The messages match
// CPython's ValueError text (minus the exception-class prefix), which is what
// users see on stderr.
func TestSelectRecommendationErrors(t *testing.T) {
	t.Parallel()

	profile, err := LoadProfilePayload(td("profile.json"))
	if err != nil {
		t.Fatalf("LoadProfilePayload: %v", err)
	}

	tests := []struct {
		name    string
		sel     SelectOptions
		wantMsg string
	}{
		{
			name:    "index past the end",
			sel:     SelectOptions{Index: intp(99)},
			wantMsg: "recommendation index 99 outside filtered range 0..6",
		},
		{
			name:    "negative index",
			sel:     SelectOptions{Index: intp(-1)},
			wantMsg: "recommendation index -1 outside filtered range 0..6",
		},
		{
			name:    "codec filter matches nothing",
			sel:     SelectOptions{Codec: "libtheora"},
			wantMsg: "encoder profile has no recommendation matching the requested filters",
		},
		{
			name:    "target filter matches nothing",
			sel:     SelectOptions{TargetVMAF: f64p(42)},
			wantMsg: "encoder profile has no recommendation matching the requested filters",
		},
		{
			// The index range is computed AFTER filtering, so a filter that
			// leaves one row rejects index 1.
			name:    "index past the filtered end",
			sel:     SelectOptions{Codec: "h264_nvenc", Index: intp(1)},
			wantMsg: "recommendation index 1 outside filtered range 0..0",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			_, err := SelectRecommendation(profile, tt.sel)
			if err == nil {
				t.Fatal("SelectRecommendation succeeded; want an error")
			}
			if err.Error() != tt.wantMsg {
				t.Errorf("error = %q, want %q", err.Error(), tt.wantMsg)
			}
		})
	}
}

// TestTargetVMAFFilterUsesIsClose pins the tolerance: the filter compares with
// math.isclose(abs_tol=1e-6), not equality, so a target that survived a
// float round-trip still matches.
func TestTargetVMAFFilterUsesIsClose(t *testing.T) {
	t.Parallel()

	profile, err := LoadProfilePayload(td("profile.json"))
	if err != nil {
		t.Fatalf("LoadProfilePayload: %v", err)
	}

	tests := []struct {
		name   string
		target float64
		want   bool
	}{
		{"exact", 92.0, true},
		{"within abs_tol", 92.0000009, true},
		{"outside abs_tol", 92.001, false},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			_, err := SelectRecommendation(profile, SelectOptions{TargetVMAF: &tt.target})
			if got := err == nil; got != tt.want {
				t.Errorf("target %v matched = %v, want %v (err=%v)", tt.target, got, tt.want, err)
			}
		})
	}
}

func TestLoadProfilePayloadErrors(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	write := func(name, content string) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte(content), 0o600); err != nil {
			t.Fatalf("write %s: %v", name, err)
		}
		return p
	}

	tests := []struct {
		name    string
		path    string
		wantSub string
	}{
		{
			name:    "missing file",
			path:    filepath.Join(dir, "nope.json"),
			wantSub: "cannot read profile",
		},
		{
			name:    "html without a pre block",
			path:    write("bare.html", "<html><body>no payload</body></html>"),
			wantSub: "does not contain a raw JSON <pre> block",
		},
		{
			name:    "markdown without a json fence",
			path:    write("bare.md", "# Report\n\nno payload\n"),
			wantSub: "does not contain a fenced JSON payload",
		},
		{
			name:    "malformed json",
			path:    write("broken.json", "{not json"),
			wantSub: "cannot parse profile JSON",
		},
		{
			name:    "wrong schema",
			path:    write("wrong.json", `{"schema": "vmaftune.encoder_profile.v0"}`),
			wantSub: "unsupported encoder profile schema",
		},
		{
			name:    "no recommendations list",
			path:    write("norecs.json", `{"schema": "`+SchemaID+`"}`),
			wantSub: "has no recommendations list",
		},
		{
			name: "recommendations is not a list",
			path: write("badrecs.json",
				`{"schema": "`+SchemaID+`", "recommendations": {}}`),
			wantSub: "has no recommendations list",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			_, err := LoadProfilePayload(tt.path)
			if err == nil {
				t.Fatal("LoadProfilePayload succeeded; want an error")
			}
			if !strings.Contains(err.Error(), tt.wantSub) {
				t.Errorf("error %q does not contain %q", err, tt.wantSub)
			}
		})
	}
}

func TestBuildEncodeRequestErrors(t *testing.T) {
	t.Parallel()

	profile, err := LoadProfilePayload(td("profile.json"))
	if err != nil {
		t.Fatalf("LoadProfilePayload: %v", err)
	}

	tests := []struct {
		name    string
		profile Profile
		rec     Recommendation
		build   BuildOptions
		wantSub string
	}{
		{
			name:    "recommendation without a codec",
			profile: profile,
			rec:     Recommendation{"crf": json.Number("23")},
			wantSub: "no codec",
		},
		{
			name:    "recommendation without a usable quality",
			profile: profile,
			rec:     Recommendation{"codec": "libx264"},
			wantSub: "no usable CRF/quality",
		},
		{
			name:    "negative quality is rejected",
			profile: profile,
			rec:     Recommendation{"codec": "libx264", "crf": json.Number("-1")},
			wantSub: "no usable CRF/quality",
		},
		{
			name:    "profile without a source path",
			profile: Profile{"recommendations": []any{}},
			rec:     Recommendation{"codec": "libx264", "crf": json.Number("23")},
			wantSub: "no source path",
		},
		{
			name: "raw source without geometry",
			profile: Profile{
				"source": map[string]any{"path": "clip.yuv"},
			},
			rec:     Recommendation{"codec": "libx264", "crf": json.Number("23")},
			wantSub: "raw sources require width, height, and framerate",
		},
		{
			name:    "unknown source kind",
			profile: profile,
			rec:     Recommendation{"codec": "libx264", "crf": json.Number("23")},
			build:   BuildOptions{SourceKind: "sideways"},
			wantSub: "unknown source kind",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			b := tt.build
			b.Output = "out.mkv"
			_, err := BuildEncodeRequest(tt.profile, tt.rec, b)
			if err == nil {
				t.Fatal("BuildEncodeRequest succeeded; want an error")
			}
			if !strings.Contains(err.Error(), tt.wantSub) {
				t.Errorf("error %q does not contain %q", err, tt.wantSub)
			}
		})
	}
}

// TestBuildEncodeRequestPresetFallback pins the preset resolution chain:
// --preset, then the recommendation's own preset, then the run block's, then
// the codec adapter's default. The literal "adapter default" is a placeholder
// some reports carry and always falls through to the adapter default.
func TestBuildEncodeRequestPresetFallback(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name     string
		rec      Recommendation
		runMeta  map[string]any
		override string
		want     string
	}{
		{
			name:     "flag wins",
			rec:      Recommendation{"codec": "libx264", "crf": json.Number("23"), "preset": "slow"},
			runMeta:  map[string]any{"preset": "fast"},
			override: "veryslow",
			want:     "veryslow",
		},
		{
			name:    "recommendation preset wins over run block",
			rec:     Recommendation{"codec": "libx264", "crf": json.Number("23"), "preset": "slow"},
			runMeta: map[string]any{"preset": "fast"},
			want:    "slow",
		},
		{
			name:    "run block is the next fallback",
			rec:     Recommendation{"codec": "libx264", "crf": json.Number("23")},
			runMeta: map[string]any{"preset": "fast"},
			want:    "fast",
		},
		{
			name: "adapter default is the last resort",
			rec:  Recommendation{"codec": "libx264", "crf": json.Number("23")},
			want: "medium",
		},
		{
			name: "the adapter default placeholder falls through",
			rec: Recommendation{
				"codec": "libx264", "crf": json.Number("23"), "preset": "adapter default",
			},
			want: "medium",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			profile := Profile{
				"source": map[string]any{"path": "clip.mp4"},
				"run":    tt.runMeta,
			}
			req, err := BuildEncodeRequest(profile, tt.rec, BuildOptions{
				Output: "out.mkv", PresetOverride: tt.override,
			})
			if err != nil {
				t.Fatalf("BuildEncodeRequest: %v", err)
			}
			if req.Preset != tt.want {
				t.Errorf("preset = %q, want %q", req.Preset, tt.want)
			}
		})
	}
}

func TestInferSourceIsContainer(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		source  string
		kind    string
		want    bool
		wantErr bool
	}{
		{name: "yuv is raw", source: "a.yuv", kind: "auto", want: false},
		{name: "raw is raw", source: "a.raw", kind: "auto", want: false},
		{name: "rgb is raw", source: "a.rgb", kind: "auto", want: false},
		{name: "gray is raw", source: "a.gray", kind: "auto", want: false},
		{name: "uppercase suffix is raw", source: "a.YUV", kind: "auto", want: false},
		{name: "mp4 is a container", source: "a.mp4", kind: "auto", want: true},
		{name: "no suffix is a container", source: "clip", kind: "auto", want: true},
		{name: "empty kind defaults to auto", source: "a.yuv", kind: "", want: false},
		{name: "explicit container overrides", source: "a.yuv", kind: "container", want: true},
		{name: "explicit raw overrides", source: "a.mp4", kind: "raw", want: false},
		{name: "unknown kind errors", source: "a.mp4", kind: "sideways", wantErr: true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			got, err := InferSourceIsContainer(tt.source, tt.kind)
			if (err != nil) != tt.wantErr {
				t.Fatalf("err = %v, wantErr = %v", err, tt.wantErr)
			}
			if err == nil && got != tt.want {
				t.Errorf("InferSourceIsContainer(%q, %q) = %v, want %v",
					tt.source, tt.kind, got, tt.want)
			}
		})
	}
}

// TestPathString pins pathlib.PurePosixPath's normalisation, which the emitted
// argv depends on. filepath.Clean is NOT a drop-in: it resolves "..", which
// pathlib deliberately leaves alone.
func TestPathString(t *testing.T) {
	t.Parallel()

	tests := []struct{ name, in, want string }{
		{"empty stays empty", "", ""},
		{"dot", ".", "."},
		{"duplicate slashes collapse", "a//b", "a/b"},
		{"dot components drop", "a/./b", "a/b"},
		{"trailing slash drops", "a/b/", "a/b"},
		{"leading dot slash drops", "./a", "a"},
		{"absolute stays absolute", "/a//b/", "/a/b"},
		{"double-slash root is preserved", "//net/share", "//net/share"},
		{"triple slash collapses to one", "///a", "/a"},
		// pathlib does NOT resolve "..", because doing so changes meaning
		// across symlinks.
		{"dotdot is preserved", "a/../b", "a/../b"},
		{"only dots becomes dot", "./.", "."},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			if got := pathString(tt.in); got != tt.want {
				t.Errorf("pathString(%q) = %q, want %q", tt.in, got, tt.want)
			}
		})
	}
}

// TestPathSuffix pins pathlib.PurePath.suffix, which decides raw-vs-container.
func TestPathSuffix(t *testing.T) {
	t.Parallel()

	tests := []struct{ name, in, want string }{
		{"simple", "a.yuv", ".yuv"},
		{"with directory", "x/y/a.mp4", ".mp4"},
		{"last suffix only", "a.tar.gz", ".gz"},
		{"no suffix", "clip", ""},
		{"dotfile has no suffix", ".bashrc", ""},
		{"trailing dot has no suffix", "clip.", ""},
		{"empty", "", ""},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			if got := pathSuffix(tt.in); got != tt.want {
				t.Errorf("pathSuffix(%q) = %q, want %q", tt.in, got, tt.want)
			}
		})
	}
}
