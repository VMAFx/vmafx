// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build unix

package execstub

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"syscall"
	"testing"
	"time"
)

// Writers and stubs per writer of the stress test (HISS-02).
const (
	writers          = 16
	stubsPerWriter   = 25
	stubRunTimeout   = 30 * time.Second
	stubScriptString = "#!/bin/sh\nexit 0\n"
)

// writeAndRun writes stubs with write and runs each at once, as a test that
// writes its vmaf stub and scores does, while the other writers fork. It
// returns how many runs failed with "text file busy".
func writeAndRun(t *testing.T, write func(string, []byte) error) int {
	t.Helper()
	dir := t.TempDir()
	ctx, cancel := context.WithTimeout(context.Background(), stubRunTimeout)
	defer cancel()
	var busy, other sync.Map
	var wg sync.WaitGroup
	for w := range writers {
		wg.Go(func() {
			for i := range stubsPerWriter {
				path := filepath.Join(dir, "stub-"+string(rune('a'+w))+"-"+string(rune('a'+i)))
				if err := write(path, []byte(stubScriptString)); err != nil {
					other.Store(path, err)
					continue
				}
				err := exec.CommandContext(ctx, path).Run() // #nosec G204 -- the stub this test wrote
				switch {
				case errors.Is(err, syscall.ETXTBSY):
					busy.Store(path, err)
				case err != nil:
					other.Store(path, err)
				}
			}
		})
	}
	wg.Wait()
	other.Range(func(k, v any) bool {
		t.Errorf("%s: %v", k, v)
		return true
	})
	n := 0
	busy.Range(func(any, any) bool { n++; return true })
	return n
}

// TestWriteRunsUnderConcurrentForks: stubs written by Write run without
// "text file busy" while other goroutines fork. With os.WriteFile in place of
// write, the same load failed 17 to 41 of the 400 runs per pass on Linux
// (go.dev/issue/22315), and as many with a temporary file closed, made
// executable and renamed into place.
func TestWriteRunsUnderConcurrentForks(t *testing.T) {
	if busy := writeAndRun(t, write); busy != 0 {
		t.Errorf("%d of %d stubs written by Write failed with text file busy", busy, writers*stubsPerWriter)
	}
}

// TestWriteMakesAnExecutable: the file holds the content and only its owner
// may write or run it.
func TestWriteMakesAnExecutable(t *testing.T) {
	path := filepath.Join(t.TempDir(), "vmaf")
	Write(t, path, []byte(stubScriptString))
	got, err := os.ReadFile(path) // #nosec G304 -- the file this test wrote
	if err != nil || string(got) != stubScriptString {
		t.Fatalf("content = %q, %v", got, err)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if mode := info.Mode().Perm(); mode != 0o700 {
		t.Errorf("mode = %o, want 700", mode)
	}
}

// TestWriteReportsAFailure: a path in a missing directory is an error that
// names the path.
func TestWriteReportsAFailure(t *testing.T) {
	path := filepath.Join(t.TempDir(), "missing", "vmaf")
	if err := write(path, []byte(stubScriptString)); err == nil {
		t.Error("write into a missing directory succeeded")
	}
}
