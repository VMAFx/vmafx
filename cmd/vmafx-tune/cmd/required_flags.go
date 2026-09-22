// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package cmd

import (
	"errors"
	"fmt"

	"github.com/spf13/cobra"
)

// markCommandFlagsRequired applies Cobra's required-flag annotations and
// turns any command-construction defect into an exit-2 validation error.
func markCommandFlagsRequired(cmd *cobra.Command, names ...string) {
	var configErr error
	for _, name := range names {
		if err := cmd.MarkFlagRequired(name); err != nil {
			configErr = errors.Join(configErr, fmt.Errorf("mark %q required: %w", name, err))
		}
	}
	if configErr == nil {
		return
	}
	cmd.Args = func(current *cobra.Command, _ []string) error {
		return &exitCodeError{code: 2, err: fmt.Errorf(
			"configure required flags for %s: %w", current.Name(), configErr)}
	}
}
