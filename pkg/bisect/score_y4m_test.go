// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package bisect

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/internal/execstub"
)

func TestY4MDecodeArgv(t *testing.T) {
	t.Parallel()
	got := strings.Join(Y4MDecodeArgv("in.mkv", "out.y4m", 0, 0), " ")
	want := "-y -hide_banner -loglevel error -i in.mkv -f yuv4mpegpipe -strict -1 out.y4m"
	if got != want {
		t.Errorf("unscaled argv = %q, want %q", got, want)
	}
	got = strings.Join(Y4MDecodeArgv("in.mp4", "out.y4m", 640, 360), " ")
	want = "-y -hide_banner -loglevel error -i in.mp4 -vf scale=640:360 -f yuv4mpegpipe -strict -1 out.y4m"
	if got != want {
		t.Errorf("scaled argv = %q, want %q", got, want)
	}
	// One positive dimension is no target geometry.
	if strings.Contains(strings.Join(Y4MDecodeArgv("a", "b", 640, 0), " "), "scale") {
		t.Error("a half-set geometry must not scale")
	}
}

func TestParseY4MHeader(t *testing.T) {
	t.Parallel()
	w, h, err := parseY4MHeader("YUV4MPEG2 W576 H324 F24:1 Ip A1:1 C420jpeg\n")
	if err != nil || w != 576 || h != 324 {
		t.Fatalf("parseY4MHeader = (%d, %d, %v), want (576, 324, nil)", w, h, err)
	}
	for _, bad := range []string{"", "RIFF W576 H324", "YUV4MPEG2 W576", "YUV4MPEG2 Wx H324", "YUV4MPEG2 W0 H1"} {
		if _, _, err := parseY4MHeader(bad); err == nil {
			t.Errorf("parseY4MHeader(%q) accepted a header without a usable geometry", bad)
		}
	}
}

func TestY4MScorer_RefusesRawYUV(t *testing.T) {
	t.Parallel()
	s := NewY4MScorer(Y4MScoreParams{VMAFBin: "/no/vmaf", FFmpegBin: "/no/ffmpeg", WorkDir: t.TempDir()})
	if _, err := s.Score("ref.yuv", "dist.mkv"); err == nil || !strings.Contains(err.Error(), "raw YUV") {
		t.Errorf("raw .yuv reference: err = %v, want a raw-YUV refusal", err)
	}
	if _, err := s.Score("ref.y4m", "dist.yuv"); err == nil || !strings.Contains(err.Error(), "raw YUV") {
		t.Errorf("raw .yuv encode: err = %v, want a raw-YUV refusal", err)
	}
}

// fakeTools writes a fake ffmpeg (writes a Y4M header of the requested scale,
// default 64x36, to its last argument and logs its argv) and a fake vmaf that,
// like the real one, refuses any input that is not .y4m and otherwise writes a
// pooled VMAF of 91.5 as XML. Both are POSIX shell scripts.
func fakeTools(t *testing.T) (dir, log string) {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("the fake ffmpeg / vmaf are POSIX shell scripts; the decode argv " +
			"is pinned platform-free by TestY4MDecodeArgv")
	}
	dir = t.TempDir()
	log = filepath.Join(dir, "calls.log")
	ffmpeg := `#!/bin/sh
echo "ffmpeg $*" >> "` + log + `"
geom="64 36"
prev=""
for a in "$@"; do
  case "$prev" in -vf) geom=$(echo "$a" | sed 's/^scale=//; s/:/ /');; esac
  prev="$a"; last="$a"
done
set -- $geom
printf 'YUV4MPEG2 W%s H%s F24:1 C420jpeg\n' "$1" "$2" > "$last"
`
	vmaf := `#!/bin/sh
echo "vmaf $*" >> "` + log + `"
out=""
while [ $# -gt 0 ]; do
  case "$1" in
    --reference|--distorted) case "$2" in *.y4m) ;; *) echo "Error opening y4m file." >&2; exit 255;; esac; shift 2;;
    --output) out="$2"; shift 2;;
    *) shift;;
  esac
done
echo '<metric name="vmaf" min="90" max="93" mean="91.5" harmonic_mean="91.4"/>' > "$out"
`
	for name, body := range map[string]string{"ffmpeg": ffmpeg, "vmaf": vmaf} {
		execstub.Write(t, filepath.Join(dir, name), []byte(body))
	}
	return dir, log
}

func readCalls(t *testing.T, log string) []string {
	t.Helper()
	data, err := os.ReadFile(log)
	if err != nil {
		t.Fatalf("read call log: %v", err)
	}
	return strings.Split(strings.TrimSpace(string(data)), "\n")
}

// TestVMAFScoreFunc_DecodesMatroskaEncode is the regression for compare and
// ladder scoring nothing: the vmaf CLI reads only Y4M (or raw YUV with
// geometry), and the scorer used to hand it the Matroska encode directly.
func TestVMAFScoreFunc_DecodesMatroskaEncode(t *testing.T) {
	dir, log := fakeTools(t)
	t.Setenv("PATH", dir+string(os.PathListSeparator)+os.Getenv("PATH"))
	work := t.TempDir()
	ref := filepath.Join(work, "ref.y4m")
	dist := filepath.Join(work, "enc.mkv")
	for _, p := range []string{ref, dist} {
		if err := os.WriteFile(p, []byte("YUV4MPEG2 W64 H36 F24:1\n"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	score, err := VMAFScoreFunc(filepath.Join(dir, "vmaf"))(ref, dist)
	if err != nil {
		t.Fatalf("score: %v", err)
	}
	if score != 91.5 {
		t.Errorf("score = %v, want 91.5", score)
	}
	calls := readCalls(t, log)
	if len(calls) != 2 || !strings.HasPrefix(calls[0], "ffmpeg ") || !strings.Contains(calls[0], "-i "+dist) {
		t.Fatalf("calls = %q, want one decode of the encode then vmaf", calls)
	}
	if !strings.Contains(calls[1], "--reference "+ref) {
		t.Errorf("a Y4M reference must reach vmaf unchanged: %q", calls[1])
	}
}

// TestY4MScorer_ScalesReferenceOnceAndCleansUp pins the ladder rung contract:
// the container reference is decoded through scale=W:H once for any number of
// probes, every decoded encode is removed after its score, and Close removes
// the decoded reference.
func TestY4MScorer_ScalesReferenceOnceAndCleansUp(t *testing.T) {
	dir, log := fakeTools(t)
	work := t.TempDir()
	ref := filepath.Join(work, "src.mp4")
	if err := os.WriteFile(ref, []byte("container"), 0o600); err != nil {
		t.Fatal(err)
	}
	s := NewY4MScorer(Y4MScoreParams{
		VMAFBin: filepath.Join(dir, "vmaf"), FFmpegBin: filepath.Join(dir, "ffmpeg"),
		WorkDir: work, Width: 320, Height: 180,
	})
	for _, name := range []string{"a.mkv", "b.mkv"} {
		dist := filepath.Join(work, name)
		if err := os.WriteFile(dist, []byte("mkv"), 0o600); err != nil {
			t.Fatal(err)
		}
		if _, err := s.Score(ref, dist); err != nil {
			t.Fatalf("score %s: %v", name, err)
		}
	}
	refDecodes := 0
	for _, c := range readCalls(t, log) {
		if strings.Contains(c, "-i "+ref) {
			refDecodes++
			if !strings.Contains(c, "-vf scale=320:180") {
				t.Errorf("reference decode not scaled to the rung: %q", c)
			}
		}
	}
	if refDecodes != 1 {
		t.Errorf("reference decoded %d times, want once", refDecodes)
	}
	assertY4MCount(t, work, 1) // the decoded reference only
	if err := s.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}
	assertY4MCount(t, work, 0)
}

// TestY4MScorer_UsesMatchingY4MReference: a Y4M reference already at the
// target geometry is scored as is; one of another size is scaled.
func TestY4MScorer_UsesMatchingY4MReference(t *testing.T) {
	dir, log := fakeTools(t)
	work := t.TempDir()
	ref := filepath.Join(work, "ref.y4m")
	if err := os.WriteFile(ref, []byte("YUV4MPEG2 W320 H180 F24:1 C420jpeg\nFRAME\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	dist := filepath.Join(work, "enc.y4m")
	if err := os.WriteFile(dist, []byte("YUV4MPEG2 W320 H180 F24:1\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	match := NewY4MScorer(Y4MScoreParams{VMAFBin: filepath.Join(dir, "vmaf"),
		FFmpegBin: filepath.Join(dir, "ffmpeg"), WorkDir: work, Width: 320, Height: 180})
	if _, err := match.Score(ref, dist); err != nil {
		t.Fatalf("score: %v", err)
	}
	for _, c := range readCalls(t, log) {
		if strings.HasPrefix(c, "ffmpeg ") {
			t.Errorf("matching Y4M pair needs no decode, got %q", c)
		}
	}
	other := NewY4MScorer(Y4MScoreParams{VMAFBin: filepath.Join(dir, "vmaf"),
		FFmpegBin: filepath.Join(dir, "ffmpeg"), WorkDir: work, Width: 160, Height: 90})
	if _, err := other.Score(ref, dist); err != nil {
		t.Fatalf("score: %v", err)
	}
	last := readCalls(t, log)
	if !strings.Contains(strings.Join(last, "\n"), "-vf scale=160:90") {
		t.Errorf("a Y4M reference of another size must be scaled: %q", last)
	}
	if err := other.Close(); err != nil {
		t.Fatal(err)
	}
}

func assertY4MCount(t *testing.T, dir string, want int) {
	t.Helper()
	matches, err := filepath.Glob(filepath.Join(dir, "vmafx-tune-*.y4m"))
	if err != nil {
		t.Fatal(err)
	}
	if len(matches) != want {
		t.Errorf("decoded Y4M files in work dir = %v, want %d", matches, want)
	}
}

// TestY4MScorer_ModelArgument: a scorer with a Model hands libvmaf that model
// as --model version=...; one without leaves the flag off so the binary picks
// its own default (what compare and bisect have always done).
func TestY4MScorer_ModelArgument(t *testing.T) {
	cases := []struct {
		name, model, want string
	}{
		{"4k model", "vmaf_v1.0.16_1d5h_2160", "--model version=vmaf_v1.0.16_1d5h_2160"},
		{"path override passes through", "path=/m/x.json", "--model path=/m/x.json"},
		{"no model, no flag", "", ""},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir, log := fakeTools(t)
			work := t.TempDir()
			ref := filepath.Join(work, "ref.y4m")
			dist := filepath.Join(work, "dist.y4m")
			for _, p := range []string{ref, dist} {
				if err := os.WriteFile(p, []byte("YUV4MPEG2 W64 H36 F24:1 C420jpeg\n"), 0o600); err != nil {
					t.Fatal(err)
				}
			}
			s := NewY4MScorer(Y4MScoreParams{
				VMAFBin: filepath.Join(dir, "vmaf"), FFmpegBin: filepath.Join(dir, "ffmpeg"),
				WorkDir: work, Model: tc.model,
			})
			if _, err := s.Score(ref, dist); err != nil {
				t.Fatalf("score: %v", err)
			}
			call := readCalls(t, log)[0]
			if tc.want == "" {
				if strings.Contains(call, "--model") {
					t.Errorf("vmaf call has --model without a Model: %q", call)
				}
				return
			}
			if !strings.Contains(call, tc.want) {
				t.Errorf("vmaf call %q lacks %q", call, tc.want)
			}
		})
	}
}
