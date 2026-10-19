// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/ebpf_config.go — configuration of the eBPF descriptor
// tracker (ADR-0779, ADR-1539):
//
//	VMAFX_EBPF_BYPASS        -> ebpf.bypass        "1" / "true" starts the tracker; empty, "0" or "false" leaves it off.
//	VMAFX_EBPF_MOUNT_PREFIX  -> ebpf.mount_prefix  Mount prefix the tracker watches (default /rclone-mount/).
//
// With the tracker on, the node refuses to start unless the storage layer
// mounts under that prefix (VMAFX_STORAGE_MODE resolving to mount, with
// VMAFX_STORAGE_MOUNT_ROOT under the prefix): a tracker that watches a path
// nothing mounts at would observe nothing.

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/VMAFx/vmafx/pkg/storage"
)

// defaultEBPFMountPrefix matches bpf.DefaultMountPrefix; repeated here so the
// configuration compiles where the bpf package does not (non-Linux).
const defaultEBPFMountPrefix = "/rclone-mount/"

// ebpfConfig is the validated tracker configuration.
type ebpfConfig struct {
	Enabled     bool
	MountPrefix string
}

// loadEBPFConfig reads the tracker keys; a value it cannot parse is an error.
func loadEBPFConfig(cfg configGetter) (ebpfConfig, error) {
	raw := strings.TrimSpace(cfg.Get("ebpf.bypass"))
	enabled := false
	if raw != "" {
		v, err := strconv.ParseBool(raw)
		if err != nil {
			return ebpfConfig{}, fmt.Errorf("ebpf.bypass=%q is not a boolean (VMAFX_EBPF_BYPASS)", raw)
		}
		enabled = v
	}
	prefix := strings.TrimSpace(cfg.Get("ebpf.mount_prefix"))
	if prefix == "" {
		prefix = defaultEBPFMountPrefix
	}
	if !strings.HasSuffix(prefix, "/") {
		prefix += "/"
	}
	return ebpfConfig{Enabled: enabled, MountPrefix: prefix}, nil
}

// checkMountAgreement refuses a tracker that would watch nothing: storage
// must mount, and its mount root must lie under the prefix.
func (c ebpfConfig) checkMountAgreement(store storage.Storage, mountRoot string) error {
	if store.Mode() != storage.ModeMount {
		return fmt.Errorf("VMAFX_EBPF_BYPASS needs the storage layer to mount (VMAFX_STORAGE_MODE=mount, "+
			"or auto on a host with FUSE); it resolved to %s, so the tracker would observe nothing", store.Mode())
	}
	if mountRoot == "" {
		mountRoot = os.TempDir()
	}
	root := filepath.Clean(mountRoot) + "/"
	if !strings.HasPrefix(root, c.MountPrefix) {
		return fmt.Errorf("VMAFX_EBPF_MOUNT_PREFIX=%s does not contain the mount root %s "+
			"(set VMAFX_STORAGE_MOUNT_ROOT under the prefix)", c.MountPrefix, root)
	}
	return nil
}
