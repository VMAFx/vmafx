// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/storage_config.go — the node's storage layer (ADR-0719,
// ADR-1526), read from the golusoris config tree:
//
//	VMAFX_STORAGE_MODE        -> storage.mode        http-serve | mount | auto (default auto).
//	VMAFX_STORAGE_MOUNT_ROOT  -> storage.mount_root  Parent of mount mode's per-job mount points (default: the temp dir).
//	VMAFX_RCLONE_BIN          -> rclone.bin          rclone binary (default "rclone" on PATH).
//	VMAFX_RCLONE_CONFIG       -> rclone.config       rclone configuration file (default: rclone's own).
//
// An unknown mode, or mount on a host without FUSE, stops the node at startup.

package main

import (
	"fmt"
	"log/slog"
	"os/exec"

	"github.com/VMAFx/vmafx/pkg/storage"
)

// provideStorage opens the configured storage layer and logs what it can
// reach: a missing rclone leaves local paths and http(s) URLs working and
// fails every rclone remote, which the log says at startup.
func provideStorage(cfg configGetter, log *slog.Logger) (storage.Storage, error) {
	raw := cfg.Get("storage.mode")
	if raw == "" {
		raw = string(storage.ModeAuto)
	}
	rclone := cfg.Get("rclone.bin")
	if rclone == "" {
		rclone = "rclone"
	}
	store, err := storage.Open(storage.Config{
		RcloneBin:    rclone,
		RcloneConfig: cfg.Get("rclone.config"),
		Mode:         storage.Mode(raw),
		MountRoot:    cfg.Get("storage.mount_root"),
		Log:          log,
	})
	if err != nil {
		return nil, fmt.Errorf("storage (VMAFX_STORAGE_MODE=%q): %w", raw, err)
	}
	if _, lookErr := exec.LookPath(rclone); lookErr != nil {
		log.Warn("rclone not found: jobs on rclone remotes will fail; local paths and http(s) URLs work",
			"rclone", rclone, "error", lookErr)
	}
	log.Info("storage layer ready", "mode", store.Mode(), "configured", raw, "rclone", rclone)
	return store, nil
}
