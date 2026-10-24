// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// pkg/y4mdecode/y4mdecode.go — decode any container input to a Y4M stream
// through ffmpeg, optionally limited to a time window (ADR-3370).
//
// The vmaf CLI reads only .y4m and raw .yuv. A node scoring a library file
// (an .mkv or .mp4) decodes it through the ffmpeg baked into its image and
// hands the Y4M stream to libvmaf.Scorer.ScoreReaders, so nothing is written
// to disk. A window (start, duration) seeks the input before decoding, so a
// ten-second sample of a long title scores without decoding the whole file.
// pkg/bisect builds its decode argv here as well: one implementation.

package y4mdecode

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Window is a validated time window in seconds. The zero value is unset: the
// whole input is decoded.
type Window struct {
	start    string // canonical decimal seconds, "" = from the first frame
	duration string // canonical decimal seconds above zero, "" = to the end
}

// ErrNoFrame is returned by a decoded stream that ended without one frame,
// for example a window that starts past the end of the input.
var ErrNoFrame = errors.New("y4mdecode: the decoded input holds no frame (window past the end?)")

// ParseWindow validates start and duration, each empty or a decimal number of
// seconds. start must be zero or above, duration above zero.
func ParseWindow(start, duration string) (Window, error) {
	var w Window
	var err error
	if w.start, err = seconds("window start", start, false); err != nil {
		return Window{}, err
	}
	if w.duration, err = seconds("window duration", duration, true); err != nil {
		return Window{}, err
	}
	return w, nil
}

// seconds parses one window bound into the canonical form ffmpeg reads.
func seconds(name, value string, positive bool) (string, error) {
	if value == "" {
		return "", nil
	}
	v, err := strconv.ParseFloat(strings.TrimSpace(value), 64)
	if err != nil || math.IsNaN(v) || math.IsInf(v, 0) {
		return "", fmt.Errorf("%s %q is not a number of seconds", name, value)
	}
	if v < 0 || (positive && v == 0) {
		bound := "zero or above"
		if positive {
			bound = "above zero"
		}
		return "", fmt.Errorf("%s %q must be %s", name, value, bound)
	}
	return strconv.FormatFloat(v, 'f', -1, 64), nil
}

// IsSet reports whether the window limits the decode.
func (w Window) IsSet() bool { return w.start != "" || w.duration != "" }

// NeedsDecode reports whether path must go through ffmpeg before the vmaf CLI
// reads it: every input that is not a Y4M or raw YUV file, and every input of
// a windowed score (the CLI cannot seek).
func NeedsDecode(path string, w Window) bool {
	if w.IsSet() {
		return true
	}
	switch strings.ToLower(filepath.Ext(path)) {
	case ".y4m", ".yuv":
		return false
	}
	return true
}

// Argv is the ffmpeg argv (without argv[0]) that decodes src to the Y4M
// target dst ("-" for stdout), seeking to the window's start, stopping after
// its duration and scaling to width x height when both are positive.
// "-strict -1" lets the yuv4mpegpipe muxer write high-bit-depth formats.
func Argv(src, dst string, width, height int, w Window) []string {
	argv := []string{"-y", "-hide_banner", "-loglevel", "error"}
	if w.start != "" {
		argv = append(argv, "-ss", w.start)
	}
	argv = append(argv, "-i", src)
	if w.duration != "" {
		argv = append(argv, "-t", w.duration)
	}
	if width > 0 && height > 0 {
		argv = append(argv, "-vf", ScaleFilter(width, height))
	}
	return append(argv, "-f", "yuv4mpegpipe", "-strict", "-1", dst)
}

// ScaleFilter is the ffmpeg filter a scaled score uses on both legs (the
// encode and the reference decode), so they share one scaler.
func ScaleFilter(width, height int) string {
	return "scale=" + strconv.Itoa(width) + ":" + strconv.Itoa(height)
}

// stderrLimit bounds the ffmpeg diagnostics a failed decode keeps.
const stderrLimit = 4096

// Stream is a running decode: ffmpeg's Y4M output. Read returns ffmpeg's
// failure, or ErrNoFrame, in place of io.EOF; Close stops ffmpeg.
type Stream struct {
	cmd       *exec.Cmd
	out       io.ReadCloser
	stderr    *limitedBuffer
	src       string
	total     int64
	headerEnd int64 // offset of the header's newline, -1 until seen
	done      bool
	err       error
}

// Open starts ffmpeg on src and returns its Y4M output. ctx bounds ffmpeg.
func Open(ctx context.Context, ffmpeg, src string, w Window) (*Stream, error) {
	if ffmpeg == "" {
		ffmpeg = "ffmpeg"
	}
	// #nosec G204 -- ffmpeg is the operator-configured binary; argv is fixed
	// flags, the validated window and the scoped source path.
	cmd := exec.CommandContext(ctx, ffmpeg, Argv(src, "-", 0, 0, w)...)
	cmd.WaitDelay = 2 * time.Second
	stderr := &limitedBuffer{limit: stderrLimit}
	cmd.Stderr = stderr
	out, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("y4mdecode: ffmpeg stdout: %w", err)
	}
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("y4mdecode: start %s: %w", ffmpeg, err)
	}
	return &Stream{cmd: cmd, out: out, stderr: stderr, src: src, headerEnd: -1}, nil
}

// Read implements io.Reader.
func (s *Stream) Read(p []byte) (int, error) {
	if s.done {
		return 0, s.err
	}
	n, err := s.out.Read(p)
	if n > 0 && s.headerEnd < 0 {
		if i := bytes.IndexByte(p[:n], '\n'); i >= 0 {
			s.headerEnd = s.total + int64(i)
		}
	}
	s.total += int64(n)
	if err == nil {
		return n, nil
	}
	s.done = true
	s.err = s.finish(err)
	return n, s.err
}

// finish waits for ffmpeg once the output ended and names the outcome.
func (s *Stream) finish(readErr error) error {
	waitErr := s.cmd.Wait()
	if waitErr != nil {
		return fmt.Errorf("y4mdecode: decode %q failed: %w: %s",
			filepath.Base(s.src), waitErr, strings.TrimSpace(s.stderr.String()))
	}
	if !errors.Is(readErr, io.EOF) {
		return fmt.Errorf("y4mdecode: read decoded %q: %w", filepath.Base(s.src), readErr)
	}
	if s.headerEnd < 0 || s.total <= s.headerEnd+1 {
		return fmt.Errorf("%w: %q", ErrNoFrame, filepath.Base(s.src))
	}
	return io.EOF
}

// Close stops ffmpeg if it still runs and reaps it.
func (s *Stream) Close() error {
	if s.done {
		return nil
	}
	s.done = true
	s.err = io.ErrClosedPipe
	var killErr error
	if err := s.cmd.Process.Kill(); err != nil && !errors.Is(err, os.ErrProcessDone) {
		killErr = fmt.Errorf("y4mdecode: stop ffmpeg: %w", err)
	}
	// ffmpeg killed above exits with a signal; only a failure to reap it is
	// an error.
	var exitErr *exec.ExitError
	if err := s.cmd.Wait(); err != nil && !errors.As(err, &exitErr) {
		return errors.Join(killErr, fmt.Errorf("y4mdecode: reap ffmpeg: %w", err))
	}
	return killErr
}

// limitedBuffer keeps the first limit bytes written to it.
type limitedBuffer struct {
	buf   bytes.Buffer
	limit int
}

func (b *limitedBuffer) Write(p []byte) (int, error) {
	if room := b.limit - b.buf.Len(); room > 0 {
		if len(p) < room {
			room = len(p)
		}
		b.buf.Write(p[:room])
	}
	return len(p), nil
}

func (b *limitedBuffer) String() string { return b.buf.String() }
