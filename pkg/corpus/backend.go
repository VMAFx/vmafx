// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/corpus/backend.go — compatibility adapters for backend selection.
//
// pkg/scorebackend owns the one backend vocabulary, probe implementation and
// strict selection policy. These wrappers preserve the corpus package's
// pre-consolidation API while callers migrate to that canonical package.

package corpus

import (
	"context"

	"github.com/VMAFx/vmafx/pkg/scorebackend"
)

// AllBackends is a compatibility snapshot of the canonical backend vocabulary.
//
// Deprecated: use scorebackend.AllBackends.
var AllBackends = scorebackend.AllBackends()

// DefaultFallbacks is a compatibility snapshot of the canonical auto chain.
//
// Deprecated: use scorebackend.DefaultFallbacks.
var DefaultFallbacks = scorebackend.DefaultFallbacks()

// BackendUnavailableError is the canonical strict-selection error.
//
// Deprecated: use scorebackend.UnavailableError.
type BackendUnavailableError = scorebackend.UnavailableError

// ParseSupportedBackends delegates to the canonical vmaf help parser.
//
// Deprecated: use scorebackend.ParseSupportedBackends.
func ParseSupportedBackends(helpText string) map[string]bool {
	return scorebackend.ParseSupportedBackends(helpText)
}

// DetectAvailableBackends delegates to the canonical bounded host probes.
//
// Deprecated: use scorebackend.Detect with scorebackend.Options.
func DetectAvailableBackends(ctx context.Context, vmafBin string, run Runner) []string {
	return scorebackend.Detect(ctx, scoreBackendOptions(vmafBin, nil, nil, run))
}

// SelectBackend delegates to the canonical strict backend selector.
//
// Deprecated: use scorebackend.Select with scorebackend.Options.
func SelectBackend(
	ctx context.Context, prefer string, fallbacks, available []string, vmafBin string, run Runner,
) (string, error) {
	return scorebackend.Select(ctx, prefer,
		scoreBackendOptions(vmafBin, fallbacks, available, run))
}

// scoreBackendOptions adapts corpus's argv-based subprocess seam to
// scorebackend's name-and-args seam. An injected corpus runner historically
// represented the whole synthetic host, so its adapter also resolves every
// probe binary without consulting the real machine's PATH.
func scoreBackendOptions(
	vmafBin string, fallbacks, available []string, run Runner,
) scorebackend.Options {
	opts := scorebackend.Options{
		VMAFBin:   vmafBin,
		Fallbacks: fallbacks,
		Available: available,
	}
	if run == nil {
		return opts
	}
	opts.LookPath = func(name string) (string, error) {
		return name, nil
	}
	opts.Run = func(ctx context.Context, name string, args ...string) (string, string, bool) {
		argv := append([]string{name}, args...)
		result := run(ctx, argv)
		return result.Stdout, result.Stderr, result.ReturnCode == 0
	}
	return opts
}
