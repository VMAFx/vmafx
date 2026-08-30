// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

package codecadapter

import (
	"context"
	"errors"
	"os/exec"
	"strings"
	"sync"
	"time"
)

// ErrAv1VideoToolboxUnavailable is returned when av1_videotoolbox is requested
// on a host whose FFmpeg does not recognise the encoder.
//
// Python raises a dedicated Av1VideoToolboxUnavailableError so callers can tell
// "this codec is not built yet" apart from "you passed a bad preset". The port
// collapsed both into a bare fmt.Errorf, leaving nothing to match on.
var ErrAv1VideoToolboxUnavailable = errors.New(
	"av1_videotoolbox awaiting upstream FFmpeg encoder support — see ADR-0339")

const (
	// Needles from codec_adapters/av1_videotoolbox.py.
	probeNotRecognizedNeedle = "is not recognized"
	probeRecognizedNeedle    = "Encoder av1_videotoolbox"
	probeTimeout             = 5 * time.Second
)

// av1VTProbe caches the runtime probe. FFmpeg's -h returns near-instantly, but
// a sweep can consult the adapter once per trial, and the answer cannot change
// within a process.
var av1VTProbe struct {
	once      sync.Once
	available bool
}

// probeAv1VideoToolboxAvailable reports whether the host's FFmpeg recognises
// av1_videotoolbox, mirroring probe_av1_videotoolbox_available().
//
// It fails CLOSED: a missing binary, a spawn error, a timeout, or output
// matching neither needle all yield false. The placeholder defaults to
// inactive on any uncertainty.
func probeAv1VideoToolboxAvailable(ffmpegBin string) bool {
	if ffmpegBin == "" {
		resolved, err := exec.LookPath("ffmpeg")
		if err != nil {
			return false
		}
		ffmpegBin = resolved
	}
	ctx, cancel := context.WithTimeout(context.Background(), probeTimeout)
	defer cancel()

	out, err := exec.CommandContext(ctx, ffmpegBin,
		"-hide_banner", "-h", "encoder=av1_videotoolbox").CombinedOutput()
	if err != nil && len(out) == 0 {
		return false
	}
	blob := string(out)
	if strings.Contains(blob, probeNotRecognizedNeedle) {
		return false
	}
	return strings.Contains(blob, probeRecognizedNeedle)
}

// av1VideoToolboxAvailable is the cached form used by the adapter.
func av1VideoToolboxAvailable() bool {
	av1VTProbe.once.Do(func() {
		av1VTProbe.available = probeAv1VideoToolboxAvailable("")
	})
	return av1VTProbe.available
}
