// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package cmd

import (
	"errors"
	"io"
	"strings"
	"testing"

	"github.com/spf13/cobra"
)

func TestMarkCommandFlagsRequiredRejectsUnknownFlagBeforePreRun(t *testing.T) {
	t.Parallel()

	preRunCalled := false
	cmd := &cobra.Command{
		Use: "test",
		PreRun: func(*cobra.Command, []string) {
			preRunCalled = true
		},
		Run: func(*cobra.Command, []string) {},
	}
	cmd.SetOut(io.Discard)
	cmd.SetErr(io.Discard)
	markCommandFlagsRequired(cmd, "missing")
	err := cmd.Execute()
	if err == nil || !strings.Contains(err.Error(), `mark "missing" required`) {
		t.Fatalf("execute error = %v, want the missing flag name", err)
	}
	var coder exitCoder
	if !errors.As(err, &coder) || coder.ExitCode() != 2 {
		t.Fatalf("execute error = %v, want exit code 2", err)
	}
	if preRunCalled {
		t.Fatal("PreRun ran before the command-construction error")
	}
}

func TestMarkCommandFlagsRequiredPreservesPreRun(t *testing.T) {
	t.Parallel()

	called := false
	cmd := &cobra.Command{Use: "test", PreRun: func(*cobra.Command, []string) {
		called = true
	}}
	cmd.Flags().String("present", "", "test flag")
	markCommandFlagsRequired(cmd, "present")
	if cmd.PreRun == nil {
		t.Fatal("mustMarkCommandFlagsRequired cleared PreRun")
	}
	cmd.PreRun(cmd, nil)
	if !called {
		t.Fatal("existing PreRun hook was not preserved")
	}
}
