// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// pkg/encoder/probe.go — ffprobe source-geometry probe.
//
// Go port of tools/vmaf-tune/src/vmaftune/report.py probe_source(). The
// per-shot tuner (ADR-0548) auto-probes width / height / framerate / frame
// count for container sources so operators do not have to pre-extract a raw
// YUV or memorise the geometry.

package encoder

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
)

// SourceInfo is the subset of ffprobe's stream + format output the tuning
// commands consume. Field names mirror the Python SourceInfo dataclass.
type SourceInfo struct {
	Path       string
	Width      int
	Height     int
	FPS        float64
	DurationS  float64
	FrameCount int
	Codec      string
	SizeBytes  int64
}

// ffprobeBin derives the ffprobe path from an ffmpeg path, mirroring
// probeBitrateKbps: a bare "ffmpeg" resolves to a bare "ffprobe" (PATH
// lookup), an explicit path resolves to its sibling.
func ffprobeBin(ffmpegPath string) string {
	if ffmpegPath == "" {
		return "ffprobe"
	}
	dir := filepath.Dir(ffmpegPath)
	if dir == "." {
		return "ffprobe"
	}
	return filepath.Join(dir, "ffprobe")
}

// ffprobePayload is the JSON shape ffprobe emits for the entry list below.
// Numeric fields arrive as strings for format-level entries, so they are
// decoded as strings and parsed explicitly.
type ffprobePayload struct {
	Streams []struct {
		Width      int    `json:"width"`
		Height     int    `json:"height"`
		RFrameRate string `json:"r_frame_rate"`
		NbFrames   string `json:"nb_frames"`
		CodecName  string `json:"codec_name"`
		Duration   string `json:"duration"`
	} `json:"streams"`
	Format struct {
		Duration string `json:"duration"`
		Size     string `json:"size"`
	} `json:"format"`
}

// ProbeSource runs ffprobe against path and returns the parsed geometry.
//
// Mirrors report.probe_source's degradation contract exactly: when ffprobe is
// missing, times out, or exits non-zero, a zero-filled SourceInfo is returned
// with the on-disk size filled in and codec "unknown" — never an error. The
// caller decides whether the zeroes are fatal (the per-shot command reports
// "could not determine source width/height" and exits 2).
//
// The ffprobe invocation is bounded by probeTimeout() (30 s default,
// VMAFX_TUNE_PROBE_TIMEOUT override) so a wedged probe cannot hang the run.
func ProbeSource(path, ffmpegPath string) SourceInfo {
	fallback := SourceInfo{
		Path:      path,
		Codec:     "unknown",
		SizeBytes: fileSize(path),
	}

	ctx := context.Background()
	cancel := func() {}
	if to := probeTimeout(); to > 0 {
		ctx, cancel = context.WithTimeout(ctx, to)
	}
	defer cancel()

	// #nosec G204 -- the binary is ffprobe, derived from the operator-supplied
	// ffmpeg path; `path` is the --src the operator named. ctx bounds runtime.
	out, err := exec.CommandContext(ctx, ffprobeBin(ffmpegPath),
		"-v", "error",
		"-select_streams", "v:0",
		"-show_entries", "stream=width,height,r_frame_rate,nb_frames,codec_name,duration",
		"-show_entries", "format=duration,size",
		"-of", "json",
		path,
	).Output()
	if err != nil {
		return fallback
	}

	var payload ffprobePayload
	if jsonErr := json.Unmarshal(out, &payload); jsonErr != nil {
		return fallback
	}
	if len(payload.Streams) == 0 {
		return fallback
	}
	s := payload.Streams[0]

	fps := parseFrameRate(s.RFrameRate)
	duration := parseFloatOr(s.Duration, parseFloatOr(payload.Format.Duration, 0))
	frames := int(parseFloatOr(s.NbFrames, 0))
	if frames == 0 {
		frames = int(duration * fps)
	}
	size := int64(parseFloatOr(payload.Format.Size, 0))
	if size == 0 {
		size = fileSize(path)
	}

	codec := s.CodecName
	if codec == "" {
		codec = "unknown"
	}
	return SourceInfo{
		Path:       path,
		Width:      s.Width,
		Height:     s.Height,
		FPS:        fps,
		DurationS:  duration,
		FrameCount: frames,
		Codec:      codec,
		SizeBytes:  size,
	}
}

// parseFrameRate parses ffprobe's "num/den" r_frame_rate string.
// Returns 0 for a malformed value or a zero denominator, matching the Python
// try/except (ValueError, ZeroDivisionError) fallback.
func parseFrameRate(s string) float64 {
	num, den, ok := strings.Cut(strings.TrimSpace(s), "/")
	if !ok {
		return parseFloatOr(s, 0)
	}
	n, nErr := strconv.ParseFloat(num, 64)
	d, dErr := strconv.ParseFloat(den, 64)
	if nErr != nil || dErr != nil || d == 0 {
		return 0
	}
	return n / d
}

// parseFloatOr parses s, returning def when it is empty or malformed.
func parseFloatOr(s string, def float64) float64 {
	v, err := strconv.ParseFloat(strings.TrimSpace(s), 64)
	if err != nil {
		return def
	}
	return v
}

// fileSize returns the on-disk size of path, or 0 when it cannot be stat'd.
func fileSize(path string) int64 {
	st, err := os.Stat(path)
	if err != nil {
		return 0
	}
	return st.Size()
}

// String renders a SourceInfo for log lines.
func (s SourceInfo) String() string {
	return fmt.Sprintf("%s %dx%d @%.3ffps %d frames (%s)",
		s.Path, s.Width, s.Height, s.FPS, s.FrameCount, s.Codec)
}
