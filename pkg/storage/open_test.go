// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// pkg/storage/open_test.go — Open refuses unknown modes, resolves auto against
// the host's FUSE support and says which mode it chose; http(s) URLs bypass
// rclone; mount points live under MountRoot.

package storage

import (
	"bytes"
	"context"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/internal/execstub"
)

// TestParseMode accepts exactly the three implemented modes.
func TestParseMode(t *testing.T) {
	t.Parallel()
	for _, ok := range []string{"http-serve", "mount", "auto", " mount "} {
		if _, err := ParseMode(ok); err != nil {
			t.Errorf("ParseMode(%q): %v", ok, err)
		}
	}
	for _, bad := range []string{"", "rclone", "HTTP-SERVE", "fuse"} {
		if _, err := ParseMode(bad); err == nil || !strings.Contains(err.Error(), "unknown mode") {
			t.Errorf("ParseMode(%q) = %v, want an unknown-mode error", bad, err)
		}
	}
}

// TestOpen_RefusesUnknownMode: the old chart value "rclone" is refused, not
// mapped to http-serve (negative).
func TestOpen_RefusesUnknownMode(t *testing.T) {
	t.Parallel()
	if _, err := Open(Config{Mode: "rclone"}); err == nil {
		t.Fatal("Open accepted mode rclone")
	}
	if _, err := Open(Config{}); err == nil {
		t.Fatal("Open accepted an empty mode")
	}
}

// withFUSE points the FUSE probe at a fake device and a fake fusermount3, or
// at nothing when present is false. Tests using it must not run in parallel.
func withFUSE(t *testing.T, present bool) {
	t.Helper()
	dir := t.TempDir()
	old := fuseDevice
	t.Cleanup(func() { fuseDevice = old })
	fuseDevice = filepath.Join(dir, "fuse")
	t.Setenv("PATH", dir)
	if !present {
		return
	}
	if err := os.WriteFile(fuseDevice, nil, 0o600); err != nil {
		t.Fatal(err)
	}
	execstub.Write(t, filepath.Join(dir, "fusermount3"), []byte("#!/bin/sh\nexit 0\n"))
}

// TestOpen_AutoResolvesAndLogs: auto becomes mount with FUSE and http-serve
// without it, and the log names the choice.
func TestOpen_AutoResolvesAndLogs(t *testing.T) {
	for present, want := range map[bool]Mode{true: ModeMount, false: ModeHTTPServe} {
		withFUSE(t, present)
		var logs bytes.Buffer
		s, err := Open(Config{Mode: ModeAuto, Log: slog.New(slog.NewTextHandler(&logs, nil))})
		if err != nil {
			t.Fatalf("Open(auto, fuse=%v): %v", present, err)
		}
		if s.Mode() != want {
			t.Errorf("fuse=%v: auto resolved to %s, want %s", present, s.Mode(), want)
		}
		if !strings.Contains(logs.String(), "storage mode auto resolved") || !strings.Contains(logs.String(), string(want)) {
			t.Errorf("fuse=%v: log %q does not name the resolved mode", present, logs.String())
		}
	}
}

// TestOpen_MountWithoutFUSEFails: an explicit mount mode on a host without
// FUSE fails at Open instead of on the first job (negative).
func TestOpen_MountWithoutFUSEFails(t *testing.T) {
	withFUSE(t, false)
	if _, err := Open(Config{Mode: ModeMount}); err == nil || !strings.Contains(err.Error(), "needs FUSE") {
		t.Fatalf("Open(mount) without FUSE = %v, want a needs-FUSE error", err)
	}
}

// TestIsHTTP covers schemes and a host-less URL (boundary).
func TestIsHTTP(t *testing.T) {
	t.Parallel()
	for uri, want := range map[string]bool{
		"http://host/a.y4m": true, "https://host:8443/a.y4m": true, "http:///a.y4m": false,
		"s3://bucket/a.y4m": false, "/abs/a.y4m": false, "remote:path/a.y4m": false,
	} {
		if got := IsHTTP(uri); got != want {
			t.Errorf("IsHTTP(%q) = %v, want %v", uri, got, want)
		}
	}
}

// TestPrepare_HTTPPassthrough: both rclone modes hand an http(s) URL back
// unchanged without starting rclone (the rclone binary does not exist).
func TestPrepare_HTTPPassthrough(t *testing.T) {
	t.Parallel()
	const uri = "https://media.example/clips/ref.y4m"
	for _, s := range []Storage{
		&HTTPServeStorage{rcloneBin: "/nonexistent/rclone", log: slog.Default()},
		&FUSEMountStorage{rcloneBin: "/nonexistent/rclone", log: slog.Default()},
	} {
		got, cleanup, err := s.Prepare(context.Background(), uri)
		if err != nil || got != uri {
			t.Errorf("%s: Prepare = %q, %v; want the URL unchanged", s.Mode(), got, err)
		}
		cleanup()
	}
}

// TestFUSEMountStorage_UsesMountRoot: the per-job mount point is created under
// MountRoot. A stand-in rclone creates the asset where the mount would expose
// it.
func TestFUSEMountStorage_UsesMountRoot(t *testing.T) {
	t.Parallel()
	bin := filepath.Join(t.TempDir(), "rclone")
	script := "#!/bin/sh\n# argv: mount <remote> <dir> ...\nmkdir -p \"$3\" && : > \"$3/ref.y4m\"\n"
	execstub.Write(t, bin, []byte(script))
	root := t.TempDir()
	s := New(Config{Mode: ModeMount, RcloneBin: bin, MountRoot: root, Log: slog.New(slog.DiscardHandler)})
	got, cleanup, err := s.Prepare(context.Background(), "remote:bucket/ref.y4m")
	if err != nil {
		t.Fatalf("Prepare: %v", err)
	}
	defer cleanup()
	if !strings.HasPrefix(got, root+string(os.PathSeparator)) || filepath.Base(got) != "ref.y4m" {
		t.Fatalf("asset path %q is not under the mount root %q", got, root)
	}
}
