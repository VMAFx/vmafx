// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/libvmaf/validate.go — the argument checks the in-process scoring paths
// share (#2434). ScoreDirect and NewStreamScorer validate the same geometry
// and model path and fail with the same typed errors; the operation name
// leads the message so a caller can tell which entry point rejected the call.

//go:build cgo

package libvmaf

import (
	"fmt"
	"os"
)

// requireGeometry rejects a frame geometry libvmaf cannot score before any
// native resource is allocated: a non-positive size, an unset or unknown pixel
// format, or a bit depth other than 8, 10 or 12. Every failure is
// ErrInvalidArgument.
func requireGeometry(operation string, width, height int, pixFmt PixelFormat, bitDepth int) error {
	if width <= 0 || height <= 0 {
		return fmt.Errorf("%s: width/height must be positive: %w", operation, ErrInvalidArgument)
	}
	switch pixFmt {
	case PixFmtYUV420P, PixFmtYUV422P, PixFmtYUV444P:
	case 0:
		return fmt.Errorf("%s: pix_fmt is required: %w", operation, ErrInvalidArgument)
	default:
		return fmt.Errorf("%s: unsupported pix_fmt %v: %w", operation, pixFmt, ErrInvalidArgument)
	}
	if bitDepth != 8 && bitDepth != 10 && bitDepth != 12 {
		return fmt.Errorf("%s: bit_depth must be 8/10/12, got %d: %w",
			operation, bitDepth, ErrInvalidArgument)
	}
	return nil
}

// requireModelPath rejects an empty model path (ErrInvalidArgument).
func requireModelPath(operation, modelPath string) error {
	if modelPath == "" {
		return fmt.Errorf("%s: model_path is required: %w", operation, ErrInvalidArgument)
	}
	return nil
}

// requireModelExists rejects a model path that does not exist
// (ErrModelNotFound, which wraps os.ErrNotExist). Any other stat failure is
// left to the model loader, which reports it with its own errno.
func requireModelExists(operation, modelPath string) error {
	if _, err := os.Stat(modelPath); os.IsNotExist(err) {
		return fmt.Errorf("%s: model %q: %w", operation, modelPath, ErrModelNotFound)
	}
	return nil
}
