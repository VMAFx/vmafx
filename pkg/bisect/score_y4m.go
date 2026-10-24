// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/bisect/score_y4m.go — the geometry-free VMAF scorer used by compare and
// ladder.
//
// The vmaf CLI reads only .y4m and raw .yuv, and raw .yuv needs explicit
// geometry flags. compare and ladder hand the scorer a reference that is
// usually a container (src.mp4) and an encode that is always a Matroska file,
// so passing both paths straight to vmaf failed on every probe ("Error opening
// y4m file ... problem with distorted file"). Y4MScorer decodes whatever is
// not already Y4M to Y4M first; a Y4M header describes its own geometry, so no
// geometry flags are needed and the y4m branch of libvmaf reads both legs.
//
// With a target geometry set (ladder rungs), the reference is decoded through
// ffmpeg's scale filter to that geometry, the way the Python ladder decodes its
// reference to the rung target (corpus._maybe_decode_reference, ADR-0501); the
// encode side scales with the same filter (bisect.Params.EncodeExtraArgs).

package bisect

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"

	"github.com/VMAFx/vmafx/pkg/y4mdecode"
)

// Y4MScoreParams configures a Y4MScorer.
type Y4MScoreParams struct {
	// VMAFBin is the vmaf binary. Defaults to "vmaf".
	VMAFBin string

	// FFmpegBin decodes non-Y4M inputs. Defaults to "ffmpeg".
	FFmpegBin string

	// WorkDir holds the decoded Y4M files. Defaults to os.TempDir().
	WorkDir string

	// Width and Height, when both positive, are the geometry the score runs
	// at: the reference is scaled to it unless it already is a Y4M file of
	// that size. Zero leaves the reference at its own geometry.
	Width, Height int

	// Model is the libvmaf model selector, as in Y4MScoreParams of the raw
	// YUV scorer: a bare version identifier is passed as "version=...", a
	// "key=value" string through unchanged. Empty leaves --model off so the
	// binary scores with its own default.
	Model string
}

// Y4MScorer scores encodes against one or more references, decoding each
// reference to Y4M at most once. It is safe for concurrent use; Close removes
// the decoded references.
type Y4MScorer struct {
	params Y4MScoreParams

	mu   sync.Mutex
	refs map[string]string // reference path -> Y4M path to score against
	owns []string          // decoded files this scorer created
}

// NewY4MScorer returns a scorer for params.
func NewY4MScorer(params Y4MScoreParams) *Y4MScorer {
	if params.VMAFBin == "" {
		params.VMAFBin = "vmaf"
	}
	if params.FFmpegBin == "" {
		params.FFmpegBin = "ffmpeg"
	}
	if params.WorkDir == "" {
		params.WorkDir = os.TempDir()
	}
	return &Y4MScorer{params: params, refs: map[string]string{}}
}

// Score returns the pooled mean VMAF of distorted against ref. It has the
// ScoreFunc signature.
func (s *Y4MScorer) Score(ref, distorted string) (float64, error) {
	refY4M, err := s.reference(ref)
	if err != nil {
		return 0, err
	}
	distY4M, cleanup, err := s.distorted(distorted)
	if err != nil {
		return 0, err
	}
	score, scoreErr := runVMAFXML(s.params.VMAFBin, refY4M, distY4M, s.params.Model)
	return score, errors.Join(scoreErr, cleanup())
}

// Close removes every reference this scorer decoded.
func (s *Y4MScorer) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	var errs []error
	for _, path := range s.owns {
		if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
			errs = append(errs, fmt.Errorf("remove decoded reference %q: %w", path, err))
		}
	}
	s.owns = nil
	s.refs = map[string]string{}
	return errors.Join(errs...)
}

// reference returns the Y4M file to score against for ref, decoding it on the
// first request.
func (s *Y4MScorer) reference(ref string) (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if path, ok := s.refs[ref]; ok {
		return path, nil
	}
	if err := refuseRawYUV(ref, "reference"); err != nil {
		return "", err
	}
	if s.referenceUsableAsIs(ref) {
		s.refs[ref] = ref
		return ref, nil
	}
	dst, err := s.decode(ref, "ref", s.params.Width, s.params.Height)
	if err != nil {
		return "", err
	}
	s.refs[ref] = dst
	s.owns = append(s.owns, dst)
	return dst, nil
}

// referenceUsableAsIs reports whether ref can be handed to vmaf unchanged: a
// Y4M file, at the target geometry when one is set.
func (s *Y4MScorer) referenceUsableAsIs(ref string) bool {
	if !isY4M(ref) {
		return false
	}
	if s.params.Width <= 0 || s.params.Height <= 0 {
		return true
	}
	w, h, err := y4mGeometry(ref)
	return err == nil && w == s.params.Width && h == s.params.Height
}

// distorted returns the Y4M file for an encode and the function that removes
// it again (a no-op when the encode already was Y4M).
func (s *Y4MScorer) distorted(path string) (string, func() error, error) {
	noop := func() error { return nil }
	if err := refuseRawYUV(path, "distorted"); err != nil {
		return "", noop, err
	}
	if isY4M(path) {
		return path, noop, nil
	}
	dst, err := s.decode(path, "dist", 0, 0)
	if err != nil {
		return "", noop, err
	}
	return dst, func() error {
		if rmErr := os.Remove(dst); rmErr != nil && !errors.Is(rmErr, os.ErrNotExist) {
			return fmt.Errorf("remove decoded encode %q: %w", dst, rmErr)
		}
		return nil
	}, nil
}

// decode writes src as a Y4M file in the work directory, scaled to width x
// height when both are positive.
func (s *Y4MScorer) decode(src, tag string, width, height int) (string, error) {
	tmp, err := os.CreateTemp(s.params.WorkDir, "vmafx-tune-"+tag+"-*.y4m")
	if err != nil {
		return "", fmt.Errorf("create decode target: %w", err)
	}
	dst := tmp.Name()
	if closeErr := tmp.Close(); closeErr != nil {
		return "", fmt.Errorf("close decode target: %w", closeErr)
	}
	argv := Y4MDecodeArgv(src, dst, width, height)
	ctx, cancel := scoreContext()
	defer cancel()
	// #nosec G204 -- FFmpegBin is operator-configured; the rest of argv is
	// fixed flags plus the caller's paths. ctx bounds the runtime.
	out, runErr := exec.CommandContext(ctx, s.params.FFmpegBin, argv...).CombinedOutput()
	if runErr != nil {
		if rmErr := os.Remove(dst); rmErr != nil && !errors.Is(rmErr, os.ErrNotExist) {
			slog.Warn("bisect: remove failed decode", "error", rmErr, "path", dst)
		}
		return "", fmt.Errorf("decode %q to Y4M: %w\n%s", src, runErr, string(out))
	}
	return dst, nil
}

// Y4MDecodeArgv is the ffmpeg argv (without argv[0]) that decodes src to the
// Y4M file dst, through the scale filter when width and height are positive.
// "-strict -1" lets the yuv4mpegpipe muxer write high-bit-depth formats.
func Y4MDecodeArgv(src, dst string, width, height int) []string {
	return y4mdecode.Argv(src, dst, width, height, y4mdecode.Window{})
}

// ScaleFilter is the ffmpeg filter both legs of a scaled score use: the encode
// (EncodeExtraArgs) and the reference decode, so they share one scaler.
func ScaleFilter(width, height int) string {
	return y4mdecode.ScaleFilter(width, height)
}

// isY4M reports whether path names a Y4M file by its extension.
func isY4M(path string) bool {
	return strings.EqualFold(filepath.Ext(path), ".y4m")
}

// refuseRawYUV rejects a headerless raw YUV input: the scorer has no geometry
// to read it with.
func refuseRawYUV(path, leg string) error {
	if rawYUVSuffixes[strings.ToLower(filepath.Ext(path))] {
		return fmt.Errorf("%s %q is raw YUV without geometry; give a .y4m file or a container", leg, path)
	}
	return nil
}

// y4mGeometry reads the W and H tokens of a Y4M stream header.
func y4mGeometry(path string) (int, int, error) {
	// #nosec G304 -- path is the caller's reference video.
	f, err := os.Open(path)
	if err != nil {
		return 0, 0, fmt.Errorf("open %q: %w", path, err)
	}
	defer func() {
		if closeErr := f.Close(); closeErr != nil {
			slog.Warn("bisect: close y4m", "error", closeErr, "path", path)
		}
	}()
	line, err := bufio.NewReaderSize(f, 512).ReadString('\n')
	if err != nil {
		return 0, 0, fmt.Errorf("read Y4M header of %q: %w", path, err)
	}
	return parseY4MHeader(line)
}

// parseY4MHeader extracts the frame geometry from a YUV4MPEG2 header line.
func parseY4MHeader(line string) (int, int, error) {
	fields := strings.Fields(line)
	if len(fields) == 0 || fields[0] != "YUV4MPEG2" {
		return 0, 0, errors.New("not a YUV4MPEG2 header")
	}
	w := y4mHeaderInt(fields[1:], 'W')
	h := y4mHeaderInt(fields[1:], 'H')
	if w <= 0 || h <= 0 {
		return 0, 0, fmt.Errorf("Y4M header without geometry: %q", strings.TrimSpace(line))
	}
	return w, h, nil
}

// y4mHeaderInt returns the integer of the first header token tagged tag, or 0
// when there is none or it does not parse.
func y4mHeaderInt(tokens []string, tag byte) int {
	for _, tok := range tokens {
		if tok[0] != tag {
			continue
		}
		n, err := strconv.Atoi(tok[1:])
		if err != nil {
			return 0
		}
		return n
	}
	return 0
}

// scoreContext bounds one decode or score subprocess by scoreTimeout().
func scoreContext() (context.Context, context.CancelFunc) {
	if to := scoreTimeout(); to > 0 {
		return context.WithTimeout(context.Background(), to)
	}
	return context.WithCancel(context.Background())
}
