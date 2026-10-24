// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package y4mdecode

import (
	"bytes"
	"context"
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"
)

func TestParseWindow(t *testing.T) {
	t.Parallel()
	cases := []struct {
		start, duration string
		want            Window
		wantErr         string
	}{
		{"", "", Window{}, ""},
		{"0", "10", Window{start: "0", duration: "10"}, ""},
		{" 1.50 ", "0.25", Window{start: "1.5", duration: "0.25"}, ""},
		{"-1", "", Window{}, "must be zero or above"},
		{"", "0", Window{}, "must be above zero"},
		{"", "-2", Window{}, "must be above zero"},
		{"abc", "", Window{}, "is not a number of seconds"},
		{"", "NaN", Window{}, "is not a number of seconds"},
		{"Inf", "", Window{}, "is not a number of seconds"},
	}
	for _, c := range cases {
		got, err := ParseWindow(c.start, c.duration)
		if c.wantErr != "" {
			if err == nil || !strings.Contains(err.Error(), c.wantErr) {
				t.Errorf("ParseWindow(%q, %q) err = %v, want %q", c.start, c.duration, err, c.wantErr)
			}
			continue
		}
		if err != nil || got != c.want {
			t.Errorf("ParseWindow(%q, %q) = %+v, %v; want %+v", c.start, c.duration, got, err, c.want)
		}
	}
}

func TestNeedsDecode(t *testing.T) {
	t.Parallel()
	win, err := ParseWindow("2", "")
	if err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		path string
		w    Window
		want bool
	}{
		{"a.y4m", Window{}, false},
		{"a.YUV", Window{}, false},
		{"a.mkv", Window{}, true},
		{"a.mp4", Window{}, true},
		{"a", Window{}, true},
		{"a.y4m", win, true},
	}
	for _, c := range cases {
		if got := NeedsDecode(c.path, c.w); got != c.want {
			t.Errorf("NeedsDecode(%q, %+v) = %v, want %v", c.path, c.w, got, c.want)
		}
	}
}

func TestArgv(t *testing.T) {
	t.Parallel()
	w, err := ParseWindow("3", "10")
	if err != nil {
		t.Fatal(err)
	}
	got := Argv("in.mkv", "-", 640, 360, w)
	want := []string{"-y", "-hide_banner", "-loglevel", "error", "-ss", "3", "-i", "in.mkv",
		"-t", "10", "-vf", "scale=640:360", "-f", "yuv4mpegpipe", "-strict", "-1", "-"}
	if !slices.Equal(got, want) {
		t.Fatalf("Argv = %q\nwant %q", got, want)
	}
	plain := Argv("in.mkv", "out.y4m", 0, 0, Window{})
	if slices.Contains(plain, "-ss") || slices.Contains(plain, "-t") || slices.Contains(plain, "-vf") {
		t.Fatalf("unwindowed, unscaled Argv carries a window or a filter: %q", plain)
	}
}

// mediaFixture encodes a one-second synthetic clip with ffmpeg's lavfi source
// and skips the test when ffmpeg or the encoder is missing.
func mediaFixture(t *testing.T, name, codec string) string {
	t.Helper()
	if _, err := exec.LookPath("ffmpeg"); err != nil {
		t.Skip("ffmpeg not on PATH")
	}
	path := filepath.Join(t.TempDir(), name)
	// #nosec G204 -- fixed test argv.
	out, err := exec.Command("ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
		"-f", "lavfi", "-i", "testsrc=size=64x48:rate=10:duration=1",
		"-pix_fmt", "yuv420p", "-c:v", codec, path).CombinedOutput()
	if err != nil {
		t.Skipf("ffmpeg cannot encode %s: %v: %s", codec, err, out)
	}
	return path
}

func decodeAll(t *testing.T, src string, w Window) ([]byte, error) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	s, err := Open(ctx, "", src, w)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer func() {
		if cerr := s.Close(); cerr != nil {
			t.Errorf("Close: %v", cerr)
		}
	}()
	return io.ReadAll(s)
}

func TestOpenDecodesContainers(t *testing.T) {
	t.Parallel()
	for _, c := range []struct{ name, codec string }{
		{"h264.mkv", "libx264"},
		{"hevc.mp4", "libx265"},
	} {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			src := mediaFixture(t, c.name, c.codec)
			data, err := decodeAll(t, src, Window{})
			if err != nil {
				t.Fatalf("decode: %v", err)
			}
			if !bytes.HasPrefix(data, []byte("YUV4MPEG2 W64 H48 ")) {
				t.Fatalf("no Y4M header: %q", data[:min(len(data), 40)])
			}
			if n := bytes.Count(data, []byte("FRAME")); n != 10 {
				t.Fatalf("frames = %d, want 10", n)
			}
			w, err := ParseWindow("0.5", "0.2")
			if err != nil {
				t.Fatal(err)
			}
			windowed, err := decodeAll(t, src, w)
			if err != nil {
				t.Fatalf("windowed decode: %v", err)
			}
			if n := bytes.Count(windowed, []byte("FRAME")); n != 2 {
				t.Fatalf("windowed frames = %d, want 2", n)
			}
		})
	}
}

func TestOpenRefusesNonMedia(t *testing.T) {
	t.Parallel()
	if _, err := exec.LookPath("ffmpeg"); err != nil {
		t.Skip("ffmpeg not on PATH")
	}
	dir := t.TempDir()
	src := filepath.Join(dir, "notes.mkv")
	if err := os.WriteFile(src, []byte("not a video\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	_, err := decodeAll(t, src, Window{})
	if err == nil || !strings.Contains(err.Error(), `decode "notes.mkv" failed`) {
		t.Fatalf("err = %v, want the decode failure", err)
	}
	entries, rerr := os.ReadDir(dir)
	if rerr != nil {
		t.Fatal(rerr)
	}
	if len(entries) != 1 {
		t.Fatalf("decode left files behind: %v", entries)
	}
}

func TestOpenWindowPastEnd(t *testing.T) {
	t.Parallel()
	src := mediaFixture(t, "short.mkv", "libx264")
	w, err := ParseWindow("30", "5")
	if err != nil {
		t.Fatal(err)
	}
	_, err = decodeAll(t, src, w)
	if !errors.Is(err, ErrNoFrame) {
		t.Fatalf("err = %v, want ErrNoFrame", err)
	}
}

func TestCloseStopsRunningDecode(t *testing.T) {
	t.Parallel()
	src := mediaFixture(t, "stop.mkv", "libx264")
	s, err := Open(context.Background(), "", src, Window{})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Read(make([]byte, 16)); err != nil {
		t.Fatalf("first read: %v", err)
	}
	if err := s.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	if _, err := s.Read(make([]byte, 16)); !errors.Is(err, io.ErrClosedPipe) {
		t.Fatalf("read after Close = %v, want io.ErrClosedPipe", err)
	}
}

func TestLimitedBuffer(t *testing.T) {
	t.Parallel()
	b := &limitedBuffer{limit: 4}
	for _, p := range []string{"ab", "cdef", "gh"} {
		if n, err := b.Write([]byte(p)); n != len(p) || err != nil {
			t.Fatalf("Write(%q) = %d, %v", p, n, err)
		}
	}
	if b.String() != "abcd" {
		t.Fatalf("kept %q, want %q", b.String(), "abcd")
	}
}
