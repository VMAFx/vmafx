// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/executor_container_test.go — the executor scores container
// inputs by decoding them with ffmpeg (pkg/y4mdecode).
//
// Positive: an H.264 .mkv pair and an HEVC .mp4 pair score; a time window
// scores a part of the clip. Negative: a file that is not media fails the job
// with the decode error and leaves no file behind; an input outside the
// scoring roots is refused before any decode. Boundary: a window past the end
// of the clip and a zero duration fail the job.

//go:build cgo

package main

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

// encodeClip writes a two-second synthetic clip in a container and skips the
// test when ffmpeg or the encoder is missing.
func encodeClip(t *testing.T, path, codec, crf string) {
	t.Helper()
	if _, err := exec.LookPath("ffmpeg"); err != nil {
		t.Skip("ffmpeg not on PATH")
	}
	// #nosec G204 -- fixed test argv.
	out, err := exec.Command("ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
		"-f", "lavfi", "-i", "testsrc=size=176x144:rate=10:duration=2",
		"-pix_fmt", "yuv420p", "-c:v", codec, "-crf", crf, path).CombinedOutput()
	if err != nil {
		t.Skipf("ffmpeg cannot encode %s: %v: %s", codec, err, out)
	}
}

// containerPair encodes a reference and a distorted clip into dir.
func containerPair(t *testing.T, dir, ext, codec string) (ref, dis string) {
	t.Helper()
	ref, dis = filepath.Join(dir, "ref"+ext), filepath.Join(dir, "dis"+ext)
	encodeClip(t, ref, codec, "18")
	encodeClip(t, dis, codec, "45")
	return ref, dis
}

func windowedJob(ref, dis, start, duration string) *controllerv1.Job {
	job := scoringJobFor(ref, dis)
	job.Scoring.WindowStart, job.Scoring.WindowDuration = start, duration
	return job
}

func TestExecuteScoring_DecodesContainers(t *testing.T) {
	for _, c := range []struct{ ext, codec string }{
		{".mkv", "libx264"},
		{".mp4", "libx265"},
	} {
		t.Run(c.codec+c.ext, func(t *testing.T) {
			ref, dis := containerPair(t, t.TempDir(), c.ext, c.codec)
			ex, _ := realExecutor(t, nil)
			res := ex.Execute(context.Background(), scoringJobFor(ref, dis))
			if res.Error != nil || res.Score <= 0 || res.Score > 100 {
				t.Fatalf("score %v (err %v), want a final score in (0, 100]", res.Score, res.Error)
			}
			win := ex.Execute(context.Background(), windowedJob(ref, dis, "0.5", "1"))
			if win.Error != nil || win.Score <= 0 || win.Score > 100 {
				t.Fatalf("windowed score %v (err %v), want a final score in (0, 100]", win.Score, win.Error)
			}
		})
	}
}

func TestExecuteScoring_NonMediaFails(t *testing.T) {
	dir := t.TempDir()
	ref, _ := containerPair(t, dir, ".mkv", "libx264")
	bogus := filepath.Join(dir, "notes.mp4")
	if err := os.WriteFile(bogus, []byte("not a video\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	ex, _ := realExecutor(t, nil)
	res := ex.Execute(context.Background(), scoringJobFor(ref, bogus))
	if res.Error == nil || !strings.Contains(res.Error.Error(), `decode "notes.mp4" failed`) {
		t.Fatalf("job error = %v, want the decode failure", res.Error)
	}
	after, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(after) != len(before) {
		t.Fatalf("job left files behind: %v -> %v", before, after)
	}
}

func TestExecuteScoring_WindowBoundaries(t *testing.T) {
	ref, dis := containerPair(t, t.TempDir(), ".mkv", "libx264")
	ex, _ := realExecutor(t, nil)
	for _, c := range []struct{ start, duration, want string }{
		{"30", "5", "no frame"},
		{"0", "0", "must be above zero"},
	} {
		res := ex.Execute(context.Background(), windowedJob(ref, dis, c.start, c.duration))
		if res.Error == nil || !strings.Contains(res.Error.Error(), c.want) {
			t.Errorf("window %s+%s: job error = %v, want %q", c.start, c.duration, res.Error, c.want)
		}
	}
}

func TestExecuteScoring_ContainerOutsideRootRefused(t *testing.T) {
	inside, outside := t.TempDir(), t.TempDir()
	ref, _ := containerPair(t, inside, ".mkv", "libx264")
	dis := filepath.Join(outside, "dis.mkv")
	encodeClip(t, dis, "libx264", "45")
	ex, _ := realExecutor(t, nil)
	job := scoringJobFor(ref, dis)
	job.ScoringRoots = []string{inside}
	res := ex.Execute(context.Background(), job)
	if res.Error == nil || !strings.Contains(res.Error.Error(), "outside the tenant's scoring roots") {
		t.Fatalf("job error = %v, want the scoring-root refusal", res.Error)
	}
	traversal := scoringJobFor(filepath.Join(inside, "..", filepath.Base(outside), "dis.mkv"), ref)
	traversal.ScoringRoots = []string{inside}
	if res := ex.Execute(context.Background(), traversal); res.Error == nil ||
		!strings.Contains(res.Error.Error(), "outside the tenant's scoring roots") {
		t.Fatalf("traversal job error = %v, want the scoring-root refusal", res.Error)
	}
}
