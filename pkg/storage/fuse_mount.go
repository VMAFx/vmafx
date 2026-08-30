// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/storage/fuse_mount.go — FUSEMountStorage implementation.
//
// FUSEMountStorage mounts a rclone remote at a per-job temporary directory via
// "rclone mount", waits for the mount to be visible, and returns the local path
// to the requested asset.  It is the fallback mode when the HTTP serve mode is
// not appropriate (e.g., the caller requires random-access seeking beyond what
// the HTTP serve mode provides).
//
// Lifecycle:
//  1. Prepare() creates a per-job tmpdir under /tmp/vmafx-rclone-<id>/.
//  2. Spawns: rclone mount <remote:root> /tmp/vmafx-rclone-<id>/ --daemon
//     [--config FILE] --vfs-cache-mode off
//  3. Polls the mountpoint until the asset path is readable.
//  4. Returns the local path /tmp/vmafx-rclone-<id>/<asset>.
//  5. cleanup() unmounts via "fusermount3 -u" (or "umount" as fallback), then
//     removes the tmpdir.
//
// FUSE availability note: distroless containers do not include the FUSE userspace
// tools by default.  The node image must install "fuse3" (Dockerfile.node includes
// this).  The HTTPServeStorage is preferred precisely because it has no FUSE
// dependency.  See ADR-0719.
//
// ADR-0719: vmafx-node rclone integration.
package storage

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

const (
	// mountReadyTimeout is the maximum time to wait for the FUSE mount to
	// become visible after the rclone mount subprocess starts.
	mountReadyTimeout = 20 * time.Second

	// mountReadyPollInterval is the interval between mount readiness polls.
	mountReadyPollInterval = 200 * time.Millisecond
)

// FUSEMountStorage mounts a rclone remote via FUSE and exposes a local path.
// It is the fallback mode; prefer HTTPServeStorage in normal operation.
type FUSEMountStorage struct {
	rcloneBin    string
	rcloneConfig string
	log          *slog.Logger
}

// Mode returns ModeMount.
func (s *FUSEMountStorage) Mode() Mode { return ModeMount }

// Prepare mounts the remote root at a temporary directory and returns the local
// path to the requested asset file.
func (s *FUSEMountStorage) Prepare(ctx context.Context, sourceURI string) (string, func(), error) {
	// Handle local paths without spawning rclone.
	if IsLocal(sourceURI) {
		lp, err := localPath(sourceURI)
		if err != nil {
			return "", func() {}, err
		}
		return lp, func() {}, nil
	}

	remotePath, err := rcloneRemotePath(sourceURI)
	if err != nil {
		return "", func() {}, err
	}

	remoteRoot, assetRel := splitRemotePath(remotePath)

	// Create per-job mount directory.
	mountDir, err := os.MkdirTemp("", "vmafx-rclone-")
	if err != nil {
		return "", func() {}, fmt.Errorf("storage: create mount dir: %w", err)
	}

	argv := s.buildMountArgs(remoteRoot, mountDir)

	s.log.Debug("starting rclone mount",
		"remote_root", remoteRoot,
		"mount_dir", mountDir,
		"asset", assetRel,
	)

	// #nosec G204 -- argv[0] is s.rcloneBin (configured at FUSEMountStorage
	// construction from operator-supplied trusted config); argv[1:] mixes
	// fixed literals (mount, --daemon, ...) with remoteRoot/mountDir already
	// validated by the caller and os.MkdirTemp respectively.
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	cmd.Stderr = os.Stderr

	if startErr := cmd.Start(); startErr != nil {
		// Join the start failure with any cleanup error so a stray empty
		// mount dir does not get silently leaked when removal also fails.
		errs := []error{fmt.Errorf("storage: start rclone mount: %w", startErr)}
		if removeErr := os.Remove(mountDir); removeErr != nil && !errors.Is(removeErr, os.ErrNotExist) {
			errs = append(errs, fmt.Errorf("storage: remove mount dir after failed start: %w", removeErr))
		}
		return "", func() {}, errors.Join(errs...)
	}

	assetPath := filepath.Join(mountDir, strings.TrimPrefix(assetRel, "/"))

	// Wait for the asset to appear on the FUSE mount.
	if readyErr := waitForPath(ctx, assetPath, mountReadyTimeout); readyErr != nil {
		// Unmount + kill + rmdir on the readiness-timeout path; surface
		// every failure via errors.Join so an operator can see whether the
		// mount actually came down or is still leaking under /tmp.
		errs := []error{fmt.Errorf("storage: rclone mount did not become ready: %w", readyErr)}
		if umErr := s.unmount(mountDir); umErr != nil {
			errs = append(errs, fmt.Errorf("storage: unmount after timeout: %w", umErr))
		}
		if killErr := killProcess(cmd); killErr != nil {
			errs = append(errs, fmt.Errorf("storage: kill rclone after timeout: %w", killErr))
		}
		if removeErr := os.RemoveAll(mountDir); removeErr != nil && !errors.Is(removeErr, os.ErrNotExist) {
			errs = append(errs, fmt.Errorf("storage: remove mount dir after timeout: %w", removeErr))
		}
		return "", func() {}, errors.Join(errs...)
	}

	cleanup := func() {
		// Cleanup runs from defer paths in production, so we can't propagate
		// errors out — log each failure but do not swallow silently.
		if umErr := s.unmount(mountDir); umErr != nil {
			s.log.Warn("storage: unmount during cleanup", "error", umErr, "mount_dir", mountDir)
		}
		if killErr := killProcess(cmd); killErr != nil {
			s.log.Warn("storage: kill rclone during cleanup", "error", killErr)
		}
		if removeErr := os.RemoveAll(mountDir); removeErr != nil && !errors.Is(removeErr, os.ErrNotExist) {
			s.log.Warn("storage: remove mount dir during cleanup", "error", removeErr, "mount_dir", mountDir)
		}
	}

	s.log.Info("rclone mount ready",
		"path", assetPath,
		"remote_root", remoteRoot,
	)

	return assetPath, cleanup, nil
}

// buildMountArgs constructs the rclone mount argument list.
func (s *FUSEMountStorage) buildMountArgs(remoteRoot, mountDir string) []string {
	argv := []string{
		s.rcloneBin, "mount", remoteRoot, mountDir,
		"--daemon",
		"--vfs-cache-mode", "off",
		"--allow-non-empty",
	}
	if s.rcloneConfig != "" {
		argv = append(argv, "--config", s.rcloneConfig)
	}
	return argv
}

// unmount unmounts the FUSE filesystem at mountDir using fusermount3 or umount
// as a fallback. Returns nil on the first success; if every attempt fails the
// joined error from all attempts is returned so the operator can see which
// binaries are missing and why.
func (s *FUSEMountStorage) unmount(mountDir string) error {
	// Try fusermount3 first (fuse3 package), then fusermount (fuse2), then umount.
	var attemptErrs []error
	for _, bin := range []string{"fusermount3", "fusermount", "umount"} {
		args := []string{"-u", mountDir}
		if bin == "umount" {
			args = []string{mountDir}
		}
		// #nosec G204 -- `bin` is one of {fusermount3, fusermount, umount} from
		// the const-string loop above; `args` is either {"-u", mountDir} or
		// {mountDir} where mountDir is the os.MkdirTemp output passed in.
		out, err := exec.Command(bin, args...).CombinedOutput()
		if err == nil {
			return nil
		}
		s.log.Debug("storage: unmount attempt", "bin", bin, "error", err, "output", string(out))
		attemptErrs = append(attemptErrs, fmt.Errorf("%s: %w", bin, err))
	}
	s.log.Warn("storage: failed to unmount FUSE mount", "mount_dir", mountDir)
	return errors.Join(attemptErrs...)
}

// waitForPath polls until path is stat-able or timeout elapses.
//
// The poll ticker is created once before the loop so each iteration reuses the
// same runtime timer — avoiding the per-iteration *time.Timer allocation that
// time.After() would create (SA1015 / ADR-1065).
func waitForPath(ctx context.Context, assetPath string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	tick := time.NewTicker(mountReadyPollInterval)
	defer tick.Stop()
	for {
		if time.Now().After(deadline) {
			return fmt.Errorf("timed out after %v waiting for %s", timeout, assetPath)
		}
		if _, err := os.Stat(assetPath); err == nil {
			return nil
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-tick.C:
		}
	}
}
