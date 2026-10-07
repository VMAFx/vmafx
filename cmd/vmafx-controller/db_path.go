// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
)

// defaultDBName is the job queue's file name under the state directory.
const defaultDBName = "vmafx-controller.db"

// stateDirs is what defaultDBPath reads from the process: the environment,
// the home directory and the user configuration directory. Tests replace it.
type stateDirs struct {
	getenv    func(string) string
	home      func() (string, error)
	configDir func() (string, error)
	goos      string
}

// processStateDirs reads the real process.
func processStateDirs() stateDirs {
	return stateDirs{getenv: os.Getenv, home: os.UserHomeDir, configDir: os.UserConfigDir, goos: runtime.GOOS}
}

// defaultDBPath is the job queue's path when db.path is unset: a vmafx
// directory under the user's state directory, never the working directory
// (a relative default wrote the queue into whatever directory the controller
// started in, a source checkout included). On Linux and the BSDs that is
// $XDG_STATE_HOME, else ~/.local/state (the XDG base directory rules: a
// relative $XDG_STATE_HOME is ignored); on macOS and Windows it is the user
// configuration directory (~/Library/Application Support, %AppData%).
func defaultDBPath(d stateDirs) (string, error) {
	dir, err := stateDir(d)
	if err != nil {
		return "", fmt.Errorf("db.path is unset and no state directory is known (%w); set VMAFX_DB_PATH", err)
	}
	return filepath.Join(dir, "vmafx", defaultDBName), nil
}

func stateDir(d stateDirs) (string, error) {
	if d.goos == "darwin" || d.goos == "windows" {
		return d.configDir()
	}
	if x := d.getenv("XDG_STATE_HOME"); filepath.IsAbs(x) {
		return x, nil
	}
	home, err := d.home()
	if err != nil {
		return "", err
	}
	if !filepath.IsAbs(home) {
		return "", errors.New("the home directory is not an absolute path")
	}
	return filepath.Join(home, ".local", "state"), nil
}

// resolveDBPath returns the configured path, or the default with its
// directory created (0700: the queue holds every tenant's jobs).
func resolveDBPath(configured string, d stateDirs) (string, error) {
	if configured != "" {
		return configured, nil
	}
	path, err := defaultDBPath(d)
	if err != nil {
		return "", err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return "", fmt.Errorf("create the job queue directory: %w", err)
	}
	return path, nil
}
