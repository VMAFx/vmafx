// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// pkg/libvmaf/readers_unix.go — score two Y4M streams without writing them to
// disk (ADR-0719 zero-materialisation contract, ADR-1526).
//
// The vmaf CLI reads its inputs sequentially, so a pipe is as good as a file.
// ScoreReaders hands the CLI the read ends of two pipes as file descriptors 3
// and 4 (`-r /dev/fd/3 -d /dev/fd/4`) and copies the caller's streams into the
// write ends. A stream that fails while it is read fails the score even when
// the CLI already returned one: the CLI cannot tell a broken stream from the
// end of the clip, so its number would describe a truncated clip.

//go:build cgo && unix

package libvmaf

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"sync/atomic"
)

// streamInput copies one caller stream into the write end of a pipe and
// records whether the stream itself failed.
type streamInput struct {
	name    string
	src     io.ReadCloser
	w       *os.File
	closing atomic.Bool // set before ScoreReaders closes src itself
	readErr atomic.Pointer[error]
	copyErr error // read after done is closed
	done    chan struct{}
}

// Read implements io.Reader for io.Copy and keeps the first read error that
// was not caused by ScoreReaders closing the stream.
func (s *streamInput) Read(p []byte) (int, error) {
	n, err := s.src.Read(p)
	if err != nil && !errors.Is(err, io.EOF) && !s.closing.Load() {
		s.readErr.CompareAndSwap(nil, &err)
	}
	return n, err
}

// pump copies until the stream ends or the CLI stops reading. A write error
// (the CLI closed its end, for example after the shorter clip ended) is not a
// stream failure.
func (s *streamInput) pump() {
	defer close(s.done)
	if _, err := io.Copy(s.w, s); err != nil {
		// A read failure is in readErr; a write failure means the CLI stopped
		// reading, and its exit status decides the outcome.
		s.copyErr = err
	}
	if err := s.w.Close(); err != nil {
		slog.Debug("libvmaf: close stream pipe", "stream", s.name, "error", err)
	}
}

// failure returns the stream's read error, if any.
func (s *streamInput) failure() error {
	if p := s.readErr.Load(); p != nil {
		return fmt.Errorf("libvmaf: %s stream failed: %w", s.name, *p)
	}
	return nil
}

// ScoreReaders scores the Y4M streams ref and dis like ScoreOnBackend scores
// two files. It takes ownership of both streams and closes them. ctx bounds
// the CLI as in Score; it does not interrupt a stream read, which closing the
// stream does once the CLI has exited.
func (s *Scorer) ScoreReaders(ctx context.Context, ref, dis io.ReadCloser, modelName, backend string) (float64, map[string]float64, error) {
	defer closeQuietly(ref)
	defer closeQuietly(dis)
	run, err := s.prepareRun(ctx, "Score", modelName)
	if err != nil {
		return 0, nil, err
	}
	defer run.release()
	argv := scoreArgv("/dev/fd/3", "/dev/fd/4", run.modelPath, run.out, backend)
	if err := s.runWithStreams(run.ctx, argv, ref, dis); err != nil {
		return 0, nil, err
	}
	return parseOutput(run.out)
}

// runWithStreams runs the CLI with the two streams on descriptors 3 and 4.
func (s *Scorer) runWithStreams(ctx context.Context, argv []string, ref, dis io.ReadCloser) error {
	refIn, refR, err := newStreamInput("reference", ref)
	if err != nil {
		return err
	}
	disIn, disR, err := newStreamInput("distorted", dis)
	if err != nil {
		closeQuietly(refR, refIn.w)
		return err
	}
	cmd, stderr := s.newCLICommand(ctx, argv)
	cmd.ExtraFiles = []*os.File{refR, disR}
	startErr := cmd.Start()
	// The child holds its own copies of the read ends; the parent's must go so
	// a CLI that exits early turns the copies' writes into EPIPE.
	closeQuietly(refR, disR)
	if startErr != nil {
		closeQuietly(refIn.w, disIn.w)
		return fmt.Errorf("libvmaf: start vmaf binary: %w", startErr)
	}
	go refIn.pump()
	go disIn.pump()
	waitErr := cmd.Wait()
	return finishStreams(ctx, waitErr, stderr, refIn, disIn)
}

// finishStreams closes both streams, waits for their copies and decides the
// outcome. A failed stream is the root cause of whatever the CLI did with the
// bytes it got, so it is reported first, joined with the CLI's failure.
func finishStreams(ctx context.Context, waitErr error, stderr *bytes.Buffer, inputs ...*streamInput) error {
	for _, in := range inputs {
		in.closing.Store(true)
		closeQuietly(in.src)
	}
	errs := make([]error, 0, len(inputs)+1)
	for _, in := range inputs {
		<-in.done
		if err := in.failure(); err != nil {
			errs = append(errs, err)
		}
	}
	if waitErr != nil {
		errs = append(errs, cliFailure(ctx, waitErr, stderr))
	}
	return errors.Join(errs...)
}

// newStreamInput wraps src and returns it with the pipe's read end.
func newStreamInput(name string, src io.ReadCloser) (*streamInput, *os.File, error) {
	r, w, err := os.Pipe()
	if err != nil {
		return nil, nil, fmt.Errorf("libvmaf: create %s pipe: %w", name, err)
	}
	return &streamInput{name: name, src: src, w: w, done: make(chan struct{})}, r, nil
}

// closeQuietly closes every non-nil closer. The callers tear down pipe ends
// and streams whose outcome is already decided, so a failure is logged, not
// returned.
func closeQuietly(closers ...io.Closer) {
	for _, c := range closers {
		if c == nil {
			continue
		}
		if err := c.Close(); err != nil && !errors.Is(err, os.ErrClosed) {
			slog.Debug("libvmaf: close stream", "error", err)
		}
	}
}
