// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build unix

package executor

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"strings"
	"testing"
)

// segvScript kills its own shell with SIGSEGV. `ulimit -c 0` keeps the dying
// shell from writing a core file into the working directory (the package
// directory under `go test`) on a host whose core_pattern is a plain name.
const segvScript = "ulimit -c 0; kill -SEGV $$; sleep 5"

// TestExecRunnerReportsSignalAsNegativeSignum pins the CPython
// subprocess.returncode convention.
//
// Go's os.ProcessState.ExitCode() collapses every signal death to -1, so a
// tool that segfaults would land -1 in tune_results.jsonl's
// score_exit_status while a Python run of the same plan recorded -11. The
// executor recovers the signal number so the two logs stay comparable — this
// is not cosmetic, it is the difference between "crashed" and "exited 1" for
// anyone triaging a results log.
func TestExecRunnerReportsSignalAsNegativeSignum(t *testing.T) {
	t.Parallel()

	sh, err := exec.LookPath("sh")
	if err != nil {
		t.Skipf("no POSIX shell available: %v", err)
	}

	tests := []struct {
		name   string
		script string
		want   int
	}{
		{"clean exit", "exit 0", 0},
		{"ordinary failure", "exit 3", 3},
		{"SIGSEGV", segvScript, -11},
		{"SIGKILL", "kill -KILL $$; sleep 5", -9},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := ExecRunner(context.Background(), []string{sh, "-c", tc.script})
			if err != nil {
				t.Fatalf("ExecRunner: %v", err)
			}
			if got.ExitCode != tc.want {
				t.Errorf("ExitCode = %d, want %d", got.ExitCode, tc.want)
			}
		})
	}
}

// coreSkipReason says why a core_pattern leaves no core file in the dying
// process's working directory, or returns "" when it does.
func coreSkipReason(pattern string) string {
	p := strings.TrimSpace(pattern)
	switch {
	case strings.HasPrefix(p, "|"):
		return "core_pattern " + p + " pipes cores to a handler, not to a file in the working directory"
	case strings.Contains(p, "/"):
		return "core_pattern " + p + " writes cores to another directory"
	case p == "":
		return "core_pattern is empty"
	}
	return ""
}

func TestCoreSkipReason(t *testing.T) {
	t.Parallel()

	for pattern, skip := range map[string]bool{
		"|/usr/lib/systemd/systemd-coredump %P %u %g %s %t %c %h\n":          true,
		"|/usr/share/apport/apport -p%p -s%s -c%c -d%d -P%P -u%u -g%g -- %E": true,
		"/var/crash/core.%e.%p": true,
		"":                      true,
		"core\n":                false,
		"core.%e.%p":            false,
	} {
		if got := coreSkipReason(pattern) != ""; got != skip {
			t.Errorf("coreSkipReason(%q) skips = %v, want %v", pattern, got, skip)
		}
	}
}

// coreFiles runs script under sh in a fresh directory with core dumps allowed
// and returns what the directory holds afterwards. It skips when the host
// cannot show a core file there: core_pattern hands cores to a program (such
// as systemd-coredump or apport) or names another directory, or the hard
// core-size limit is zero.
func coreFiles(t *testing.T, sh, script string) []os.DirEntry {
	t.Helper()
	pattern, err := os.ReadFile("/proc/sys/kernel/core_pattern")
	if err != nil {
		t.Skipf("no /proc/sys/kernel/core_pattern (%v): cannot tell where cores go", err)
	}
	if reason := coreSkipReason(string(pattern)); reason != "" {
		t.Skip(reason)
	}
	dir := t.TempDir()
	cmd := exec.Command(sh, "-c", "ulimit -c unlimited || exit 97; "+script)
	cmd.Dir = dir
	var exit *exec.ExitError
	if err := cmd.Run(); errors.As(err, &exit) && exit.ExitCode() == 97 {
		t.Skip("the hard core-size limit is below unlimited on this host")
	} else if err != nil && !errors.As(err, &exit) {
		t.Fatalf("run %q: %v", script, err)
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("read %s: %v", dir, err)
	}
	return entries
}

// TestSegvScriptLeavesNoCoreFile proves the `ulimit -c 0` in segvScript: the
// same kill without it leaves a core file in the working directory, the
// script the signal test runs leaves none.
func TestSegvScriptLeavesNoCoreFile(t *testing.T) {
	t.Parallel()

	sh, err := exec.LookPath("sh")
	if err != nil {
		t.Skipf("no POSIX shell available: %v", err)
	}
	if got := coreFiles(t, sh, "kill -SEGV $$; sleep 5"); len(got) == 0 {
		t.Fatal("the kill without `ulimit -c 0` left no core file; the check proves nothing")
	}
	if got := coreFiles(t, sh, segvScript); len(got) != 0 {
		t.Errorf("segvScript left %d file(s), first %q", len(got), got[0].Name())
	}
}

// TestExecRunnerCapturesBothStreams pins the CommandResult contract the
// encoder-version probe depends on: `ffmpeg -version` prints its configure
// summary on stdout, while encoders print their banners on stderr.
func TestExecRunnerCapturesBothStreams(t *testing.T) {
	t.Parallel()

	sh, err := exec.LookPath("sh")
	if err != nil {
		t.Skipf("no POSIX shell available: %v", err)
	}
	got, err := ExecRunner(context.Background(),
		[]string{sh, "-c", "echo to-stdout; echo to-stderr >&2"})
	if err != nil {
		t.Fatalf("ExecRunner: %v", err)
	}
	if got.Stdout != "to-stdout\n" {
		t.Errorf("Stdout = %q, want %q", got.Stdout, "to-stdout\n")
	}
	if got.Stderr != "to-stderr\n" {
		t.Errorf("Stderr = %q, want %q", got.Stderr, "to-stderr\n")
	}
}

// TestExecRunnerRejectsEmptyArgv covers the guard.
func TestExecRunnerRejectsEmptyArgv(t *testing.T) {
	t.Parallel()

	if _, err := ExecRunner(context.Background(), nil); err == nil {
		t.Error("expected an error for an empty argv")
	}
}
