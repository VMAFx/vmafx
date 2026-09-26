// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package scoringservice

import (
	"errors"
	"fmt"
)

// StreamScorerCloseResult records both bounded teardown attempts. RetryErr is
// meaningful only when InitialErr is non-nil; a nil RetryErr then means the
// immediate retry recovered the teardown failure.
type StreamScorerCloseResult struct {
	InitialErr error
	RetryErr   error
}

// Err reports a persistent teardown failure while retaining both attempt
// errors for errors.Is/errors.As callers. A recovered retry is not an error.
func (r StreamScorerCloseResult) Err() error {
	if r.InitialErr == nil || r.RetryErr == nil {
		return nil
	}
	return errors.Join(
		fmt.Errorf("initial close: %w", r.InitialErr),
		fmt.Errorf("retry close: %w", r.RetryErr),
	)
}

// CloseStreamScorerWithRetry closes a request-scoped scorer and makes at most
// one immediate retry. RPC handlers cannot return the scorer to their caller,
// so a bounded retry is their last safe opportunity to complete teardown.
func CloseStreamScorerWithRetry(scorer interface{ Close() error }) StreamScorerCloseResult {
	result := StreamScorerCloseResult{InitialErr: scorer.Close()}
	if result.InitialErr != nil {
		result.RetryErr = scorer.Close()
	}
	return result
}
