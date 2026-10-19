// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package main

import (
	"errors"
	"os"
	"path/filepath"
	"testing"

	"go.uber.org/fx"
	"go.uber.org/fx/fxtest"
)

func fakeDirs(xdg, home string, homeErr error, cfg, goos string) stateDirs {
	return stateDirs{
		getenv: func(k string) string {
			if k == "XDG_STATE_HOME" {
				return xdg
			}
			return ""
		},
		home:      func() (string, error) { return home, homeErr },
		configDir: func() (string, error) { return cfg, nil },
		goos:      goos,
	}
}

func TestDefaultDBPath(t *testing.T) {
	t.Parallel()
	root := t.TempDir() // absolute on every platform
	state, home, cfg := filepath.Join(root, "state"), filepath.Join(root, "home"), filepath.Join(root, "cfg")
	cases := []struct {
		name string
		d    stateDirs
		want string
	}{
		{"XDG_STATE_HOME", fakeDirs(state, home, nil, cfg, "linux"), filepath.Join(state, "vmafx", defaultDBName)},
		{"home fallback", fakeDirs("", home, nil, cfg, "linux"), filepath.Join(home, ".local", "state", "vmafx", defaultDBName)},
		{"relative XDG_STATE_HOME ignored", fakeDirs("state", home, nil, cfg, "freebsd"), filepath.Join(home, ".local", "state", "vmafx", defaultDBName)},
		{"macOS configuration directory", fakeDirs(state, home, nil, cfg, "darwin"), filepath.Join(cfg, "vmafx", defaultDBName)},
		{"Windows configuration directory", fakeDirs("", "", nil, cfg, "windows"), filepath.Join(cfg, "vmafx", defaultDBName)},
	}
	for _, tc := range cases {
		got, err := defaultDBPath(tc.d)
		if err != nil || got != tc.want {
			t.Errorf("%s: defaultDBPath = %q, %v; want %q", tc.name, got, err, tc.want)
		}
	}
}

// TestDefaultDBPathRefusesWithoutAStateDirectory is the negative case: no
// absolute home and no XDG_STATE_HOME is a startup error naming
// VMAFX_DB_PATH, never a fallback to the working directory.
func TestDefaultDBPathRefusesWithoutAStateDirectory(t *testing.T) {
	t.Parallel()
	for name, d := range map[string]stateDirs{
		"no home":       fakeDirs("", "", errors.New("$HOME is not defined"), "", "linux"),
		"relative home": fakeDirs("", "home", nil, "", "linux"),
	} {
		if got, err := defaultDBPath(d); err == nil {
			t.Errorf("%s: defaultDBPath = %q, want an error", name, got)
		}
	}
}

// TestJobQueueDefaultIsNotTheWorkingDirectory starts the controller graph
// without VMAFX_DB_PATH: the queue is created under the state directory and
// nothing is written to the working directory (the old default wrote
// vmafx-controller.db there, which is how three such files were committed).
func TestJobQueueDefaultIsNotTheWorkingDirectory(t *testing.T) {
	writeControllerEnv(t)
	t.Setenv("VMAFX_DB_PATH", "")
	state := t.TempDir()
	t.Setenv("XDG_STATE_HOME", state)
	wd := t.TempDir()
	t.Chdir(wd)

	app := fxtest.New(t, productionGraph(), fx.NopLogger)
	app.RequireStart()
	app.RequireStop()

	if _, err := os.Stat(filepath.Join(state, "vmafx", defaultDBName)); err != nil {
		t.Errorf("job queue not under the state directory: %v", err)
	}
	if entries, err := os.ReadDir(wd); err != nil || len(entries) != 0 {
		t.Errorf("the working directory holds %d entries (%v), want none", len(entries), err)
	}
}
