// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/executor_inputs.go — turn a scoring job's source URIs into
// scorer inputs through pkg/storage (ADR-0719, ADR-1526).
//
// The storage layer returns, per source, either a local path (a local file or
// a FUSE mount of an rclone remote) or an http(s) URL (an rclone serve http
// instance, or the source itself when it is already a URL). Two paths go to
// the vmaf CLI as files. When either input is a URL, both are streamed into
// the CLI through pipes (libvmaf.Scorer.ScoreReaders): nothing is written to
// the node's disk, and a stream that breaks fails the job.
//
// The CLI reads only Y4M and raw YUV. Any other input (a library's .mkv or
// .mp4), and every input of a job with a time window, is decoded by the
// node's ffmpeg to a Y4M stream through pkg/y4mdecode and piped in the same
// way, again without a temporary file. The scoring-root check above runs on
// the source path before any decode.

package main

import (
	"context"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/scoringscope"
	"github.com/VMAFx/vmafx/pkg/storage"
	"github.com/VMAFx/vmafx/pkg/y4mdecode"
)

// inputHeaderTimeout bounds the wait for the response headers of a streamed
// input; the body is bounded by the scoring deadline, which closes it.
const inputHeaderTimeout = 30 * time.Second

// inputClient fetches streamed inputs.
var inputClient = &http.Client{Transport: &http.Transport{
	Proxy:                 http.ProxyFromEnvironment,
	ResponseHeaderTimeout: inputHeaderTimeout,
	TLSHandshakeTimeout:   inputHeaderTimeout,
}}

// scoreJob admits both sources under the job's scoring roots, prepares them,
// scores them on the job's backend and releases the storage resources (rclone
// processes, mounts) afterwards.
func (e *Executor) scoreJob(ctx context.Context, job *controllerv1.Job) (float64, map[string]float64, error) {
	sp := job.GetScoring()
	refSrc, disSrc, err := scopedSources(job)
	if err != nil {
		return 0, nil, err
	}
	ref, refDone, err := e.store.Prepare(ctx, refSrc)
	if err != nil {
		return 0, nil, fmt.Errorf("prepare reference %q: %w", refSrc, err)
	}
	defer refDone()
	dis, disDone, err := e.store.Prepare(ctx, disSrc)
	if err != nil {
		return 0, nil, fmt.Errorf("prepare distorted %q: %w", disSrc, err)
	}
	defer disDone()
	window, err := y4mdecode.ParseWindow(sp.GetWindowStart(), sp.GetWindowDuration())
	if err != nil {
		return 0, nil, err
	}
	backend := jobBackend(sp, e.backend)
	decode := y4mdecode.NeedsDecode(ref, window) || y4mdecode.NeedsDecode(dis, window)
	if !decode && !storage.IsHTTP(ref) && !storage.IsHTTP(dis) {
		return e.scorer.ScoreOnBackend(ctx, ref, dis, sp.GetModel(), backend)
	}
	refIn, err := e.openScored(ctx, ref, decode, window)
	if err != nil {
		return 0, nil, fmt.Errorf("open reference: %w", err)
	}
	disIn, err := e.openScored(ctx, dis, decode, window)
	if err != nil {
		closeInput(e.log, refIn)
		return 0, nil, fmt.Errorf("open distorted: %w", err)
	}
	return e.scorer.ScoreReaders(ctx, refIn, disIn, sp.GetModel(), backend)
}

// openScored opens one prepared input for ScoreReaders: decoded to Y4M by
// ffmpeg when the job decodes (both legs then share the window), else as is.
// ffmpeg reads a local path or an http(s) URL itself. A raw .yuv carries no
// geometry ffmpeg could read, so it is refused rather than misdecoded.
func (e *Executor) openScored(ctx context.Context, input string, decode bool, w y4mdecode.Window) (io.ReadCloser, error) {
	if !decode {
		return openInput(ctx, input)
	}
	if strings.EqualFold(filepath.Ext(input), ".yuv") {
		return nil, fmt.Errorf("%s: a raw .yuv input cannot be decoded or windowed; use .y4m or a container", filepath.Base(input))
	}
	return y4mdecode.Open(ctx, e.ffmpeg, input, w)
}

// scopedSources admits the job's inputs under the scoring roots the
// controller sent with it (ADR-1577) and returns what to read: the real path
// of a local input (symlinks followed on this node, where the files are), a
// remote input unchanged. A job without roots is refused (deny by default).
func scopedSources(job *controllerv1.Job) (string, string, error) {
	roots, err := scoringscope.Parse(job.GetScoringRoots())
	if err != nil {
		return "", "", fmt.Errorf("scoring roots from the controller: %w", err)
	}
	sp := job.GetScoring()
	ref, err := roots.Resolve(sp.GetReference())
	if err != nil {
		return "", "", fmt.Errorf("reference: %w", err)
	}
	dis, err := roots.Resolve(sp.GetDistorted())
	if err != nil {
		return "", "", fmt.Errorf("distorted: %w", err)
	}
	return ref, dis, nil
}

// openInput opens a prepared input as a stream: an http(s) URL with a GET,
// anything else as a local file.
func openInput(ctx context.Context, input string) (io.ReadCloser, error) {
	if !storage.IsHTTP(input) {
		f, err := os.Open(input) // #nosec G304 -- the path comes from the job's storage preparation
		if err != nil {
			return nil, fmt.Errorf("open %s: %w", input, err)
		}
		return f, nil
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, input, nil)
	if err != nil {
		return nil, fmt.Errorf("request %s: %w", input, err)
	}
	resp, err := inputClient.Do(req) // #nosec G107 -- URL from the job's storage preparation
	if err != nil {
		return nil, fmt.Errorf("GET %s: %w", input, err)
	}
	if resp.StatusCode != http.StatusOK {
		closeInput(slog.Default(), resp.Body)
		return nil, fmt.Errorf("GET %s: status %s", input, resp.Status)
	}
	return resp.Body, nil
}

// closeInput closes an input that will not be scored; the job already fails
// for another reason, so a close failure is only logged.
func closeInput(log *slog.Logger, in io.Closer) {
	if err := in.Close(); err != nil {
		log.Warn("close unused scoring input", "error", err)
	}
}
