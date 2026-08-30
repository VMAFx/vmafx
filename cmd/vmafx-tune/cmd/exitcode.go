// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"errors"

	"github.com/spf13/cobra"
)

// exitCodeError carries a specific process exit status out of a subcommand.
//
// Most vmafx-tune subcommands only distinguish success from failure, and
// cobra's plain error return (exit 1) says everything they need to. One does
// not: `encode-profile` runs FFmpeg and its own exit status IS FFmpeg's, the
// way the Python CLI returns int(result.exit_status) from main(). A caller
// scripting around it distinguishes "ffmpeg reported 234" from "the CLI itself
// failed", so collapsing every failure onto 1 would lose information the
// Python command carried.
//
// Execute unwraps this and calls os.Exit with the carried status.
type exitCodeError struct {
	code int
	err  error
}

// Error renders the wrapped message.
func (e exitCodeError) Error() string { return e.err.Error() }

// Unwrap exposes the underlying error to errors.Is / errors.As.
func (e exitCodeError) Unwrap() error { return e.err }

// exitCodeOf returns the process exit status an error asks for, defaulting to
// 1 for any error that does not carry one. A nil error means success.
func exitCodeOf(err error) int {
	if err == nil {
		return 0
	}
	var ece exitCodeError
	if errors.As(err, &ece) && ece.code != 0 {
		return ece.code
	}
	return 1
}

// usageExitCode is what the Python CLI returns for a validation or usage
// failure: every `vmaf-tune` subcommand writes a diagnostic to stderr and
// `return 2`, and argparse itself exits 2 on a bad flag.
const usageExitCode = 2

// useUsageExitCode makes cmd report flag-layer failures — an unknown flag, a
// value that will not parse — with the same exit status as its own validation
// failures. Cobra raises those inside ParseFlags, before RunE ever runs, so
// wrapping the domain function is not enough on its own.
//
// The companion half is that these commands do NOT call MarkFlagRequired:
// cobra's required-flag check also runs before RunE and returns a bare error.
// Each command re-checks its required flags at the top of its own run function
// instead, so a missing flag takes the same path as any other validation
// failure.
func useUsageExitCode(cmd *cobra.Command) {
	cmd.SetFlagErrorFunc(func(_ *cobra.Command, err error) error {
		return asUsageError(err)
	})
}

// asUsageError tags a validation failure with the exit status the Python CLI
// would have used, leaving an error that already carries one alone.
//
// Scope note: only the subcommands ported with this helper honour it. The
// earlier ports (compare, ladder, report) still surface every failure as exit
// 1, so a script that branches on the status sees 2 from `benchmark` /
// `encode-profile` and 1 from the other three. That inconsistency is
// pre-existing and deliberately not widened here — making the older commands
// match Python changes their published contract and belongs in its own change.
func asUsageError(err error) error {
	if err == nil {
		return nil
	}
	var ece exitCodeError
	if errors.As(err, &ece) {
		return err
	}
	return exitCodeError{code: usageExitCode, err: err}
}
