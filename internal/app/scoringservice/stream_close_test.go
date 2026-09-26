// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package scoringservice

import (
	"errors"
	"testing"
)

type scriptedStreamCloser struct {
	errs  []error
	calls int
}

func (c *scriptedStreamCloser) Close() error {
	err := c.errs[c.calls]
	c.calls++
	return err
}

func TestCloseStreamScorerWithRetry(t *testing.T) {
	firstErr := errors.New("first close failed")
	retryErr := errors.New("retry close failed")
	tests := []struct {
		name         string
		errs         []error
		wantCalls    int
		wantInitial  error
		wantRetry    error
		wantFinalErr bool
	}{
		{
			name:      "first attempt succeeds",
			errs:      []error{nil},
			wantCalls: 1,
		},
		{
			name:        "immediate retry recovers",
			errs:        []error{firstErr, nil},
			wantCalls:   2,
			wantInitial: firstErr,
		},
		{
			name:         "persistent failure stops after one retry",
			errs:         []error{firstErr, retryErr},
			wantCalls:    2,
			wantInitial:  firstErr,
			wantRetry:    retryErr,
			wantFinalErr: true,
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			closer := &scriptedStreamCloser{errs: test.errs}
			result := CloseStreamScorerWithRetry(closer)

			if closer.calls != test.wantCalls {
				t.Fatalf("Close calls = %d, want %d", closer.calls, test.wantCalls)
			}
			if !errors.Is(result.InitialErr, test.wantInitial) {
				t.Errorf("InitialErr = %v, want %v", result.InitialErr, test.wantInitial)
			}
			if !errors.Is(result.RetryErr, test.wantRetry) {
				t.Errorf("RetryErr = %v, want %v", result.RetryErr, test.wantRetry)
			}
			finalErr := result.Err()
			if (finalErr != nil) != test.wantFinalErr {
				t.Fatalf("Err() = %v, want error=%t", finalErr, test.wantFinalErr)
			}
			if test.wantFinalErr {
				if !errors.Is(finalErr, firstErr) || !errors.Is(finalErr, retryErr) {
					t.Errorf("Err() = %v, want both close errors preserved", finalErr)
				}
			}
		})
	}
}
