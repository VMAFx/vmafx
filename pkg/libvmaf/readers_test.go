// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// pkg/libvmaf/readers_test.go — ScoreReaders: the streams reach the CLI byte
// for byte, the score equals the file score, and a stream that breaks fails
// the score instead of yielding the number of a truncated clip.

//go:build cgo && unix

package libvmaf

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/internal/execstub"
	"github.com/VMAFx/vmafx/internal/vmaftest"
)

// synthY4M returns a deterministic 4:2:0 clip; noise > 0 perturbs the luma.
func synthY4M(w, h, frames, noise int) []byte {
	var buf bytes.Buffer
	fmt.Fprintf(&buf, "YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C420jpeg\n", w, h)
	for f := range frames {
		buf.WriteString("FRAME\n")
		for y := range h {
			for x := range w {
				v := (x*3 + y*5 + f*7 + (x*y)%23) % 256
				if noise > 0 && (x+y+f)%3 == 0 {
					v = (v + noise) % 256
				}
				buf.WriteByte(byte(v))
			}
		}
		buf.Write(bytes.Repeat([]byte{128}, 2*(w/2)*(h/2)))
	}
	return buf.Bytes()
}

// failingReader yields data, then err.
type failingReader struct {
	data []byte
	err  error
}

func (f *failingReader) Read(p []byte) (int, error) {
	if len(f.data) == 0 {
		return 0, f.err
	}
	n := copy(p, f.data)
	f.data = f.data[n:]
	return n, nil
}

func (f *failingReader) Close() error { return nil }

func readCloser(b []byte) io.ReadCloser { return io.NopCloser(bytes.NewReader(b)) }

// TestScoreReaders_MatchesFileScore: streaming the clips gives the CLI's file
// score exactly (positive).
func TestScoreReaders_MatchesFileScore(t *testing.T) {
	bin := vmaftest.Binary(t)
	s, err := New(bin, filepath.Join(RepoRoot(), "model"))
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ref, dis := synthY4M(320, 240, 5, 0), synthY4M(320, 240, 5, 40)
	dir := t.TempDir()
	refPath, disPath := filepath.Join(dir, "ref.y4m"), filepath.Join(dir, "dis.y4m")
	if err := os.WriteFile(refPath, ref, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(disPath, dis, 0o600); err != nil {
		t.Fatal(err)
	}
	want, _, err := s.ScoreOnBackend(context.Background(), refPath, disPath, "vmaf_v0.6.1", "cpu")
	if err != nil {
		t.Fatalf("file score: %v", err)
	}
	got, features, err := s.ScoreReaders(context.Background(), readCloser(ref), readCloser(dis), "vmaf_v0.6.1", "cpu")
	if err != nil {
		t.Fatalf("ScoreReaders: %v", err)
	}
	if got != want || len(features) == 0 {
		t.Fatalf("stream score %v (features %d), want the file score %v", got, len(features), want)
	}
}

// TestScoreReaders_BrokenStreamFails: a reference that breaks after two frames
// fails the score, although the CLI could score the frames it got (negative).
func TestScoreReaders_BrokenStreamFails(t *testing.T) {
	bin := vmaftest.Binary(t)
	s, err := New(bin, filepath.Join(RepoRoot(), "model"))
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ref, dis := synthY4M(320, 240, 5, 0), synthY4M(320, 240, 5, 40)
	broken := &failingReader{data: ref[:len(ref)*2/5], err: io.ErrUnexpectedEOF}
	_, _, err = s.ScoreReaders(context.Background(), broken, readCloser(dis), "vmaf_v0.6.1", "cpu")
	if err == nil || !errors.Is(err, io.ErrUnexpectedEOF) || !strings.Contains(err.Error(), "reference stream failed") {
		t.Fatalf("ScoreReaders error = %v, want the reference stream failure", err)
	}
}

// TestScoreReaders_StreamBrokenAtFrameBoundaryFails: a stream that breaks
// right after a whole frame would let the CLI score the frames before it; the
// score still fails (boundary of the truncation case).
func TestScoreReaders_StreamBrokenAtFrameBoundaryFails(t *testing.T) {
	s, err := New(vmaftest.Binary(t), filepath.Join(RepoRoot(), "model"))
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	ref, dis := synthY4M(320, 240, 5, 0), synthY4M(320, 240, 5, 40)
	header := bytes.IndexByte(ref, '\n') + 1
	frame := len("FRAME\n") + 320*240*3/2
	broken := &failingReader{data: ref[:header+2*frame], err: errors.New("connection reset")}
	_, _, err = s.ScoreReaders(context.Background(), readCloser(dis), broken, "vmaf_v0.6.1", "cpu")
	if err == nil || !strings.Contains(err.Error(), "distorted stream failed: connection reset") {
		t.Fatalf("ScoreReaders error = %v, want the distorted stream failure", err)
	}
}

// TestScoreReaders_EmptyStreamsFail: two empty streams are no clip (boundary).
func TestScoreReaders_EmptyStreamsFail(t *testing.T) {
	s, err := New(vmaftest.Binary(t), filepath.Join(RepoRoot(), "model"))
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if _, _, err := s.ScoreReaders(context.Background(), readCloser(nil), readCloser(nil), "vmaf_v0.6.1", "cpu"); err == nil {
		t.Fatal("ScoreReaders scored two empty streams")
	}
}

// TestScoreReaders_PassesDescriptors: the CLI is called with /dev/fd/3 and
// /dev/fd/4 and reads exactly the bytes of each stream.
func TestScoreReaders_PassesDescriptors(t *testing.T) {
	dir := t.TempDir()
	script := `#!/bin/sh
out=""; ref=""; dis=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift 2 ;;
    -r) ref="$2"; shift 2 ;;
    -d) dis="$2"; shift 2 ;;
    *) shift ;;
  esac
done
cat "$ref" > "` + dir + `/ref.got"
cat "$dis" > "` + dir + `/dis.got"
printf '%s\n%s\n' "$ref" "$dis" > "` + dir + `/paths"
cat > "$out" <<'EOF'
` + goldenJSON + `
EOF
`
	bin := filepath.Join(dir, "vmaf")
	execstub.Write(t, bin, []byte(script))
	modelDir := t.TempDir()
	writeModel(t, modelDir, "vmaf_v0.6.1")
	s, err := New(bin, modelDir)
	if err != nil {
		t.Fatal(err)
	}
	ref, dis := bytes.Repeat([]byte("r"), 300000), []byte("distorted-bytes")
	if _, _, err := s.ScoreReaders(context.Background(), readCloser(ref), readCloser(dis), "vmaf_v0.6.1", ""); err != nil {
		t.Fatalf("ScoreReaders: %v", err)
	}
	for name, want := range map[string][]byte{"ref.got": ref, "dis.got": dis, "paths": []byte("/dev/fd/3\n/dev/fd/4\n")} {
		got, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil || !bytes.Equal(got, want) {
			t.Errorf("%s: %d bytes (err %v), want %d bytes", name, len(got), err, len(want))
		}
	}
}
