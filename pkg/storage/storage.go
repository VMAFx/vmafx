// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/storage/storage.go — rclone-backed remote storage interface.
//
// The Storage interface abstracts how a vmafx-node obtains a readable URL for
// a remote asset (S3 object, GCS blob, SFTP file, local path, etc.) and
// exposes it to ffmpeg without materialising the content to disk first.
//
// Three implementations are provided:
//   - HTTPServeStorage  — spawns "rclone serve http" per job; returns
//     http://localhost:PORT/<path>. Primary mode.
//   - FUSEMountStorage  — spawns "rclone mount" to a per-job directory;
//     returns a local POSIX path. Fallback mode.
//   - LocalStorage      — passthrough for file:// or bare paths; no rclone.
//
// The correct implementation is selected by New() based on the source URI
// scheme and the configured mode.
//
// ADR-0719: vmafx-node rclone integration (Phase 4b.5).
// ADR-0709: Phase 4b umbrella.
package storage

import (
	"context"
	"fmt"
	"log/slog"
	"net"
	"net/url"
	"strings"
)

// Mode selects the rclone access strategy.
type Mode string

const (
	// ModeHTTPServe uses "rclone serve http" to expose the remote as an HTTP
	// server. ffmpeg reads via http://localhost:PORT/path. Recommended for
	// multi-input jobs (ref + dis simultaneously).
	ModeHTTPServe Mode = "http-serve"

	// ModeMount uses "rclone mount" to expose the remote as a FUSE filesystem.
	// ffmpeg reads from a local directory path. Fallback for sources that
	// require random access beyond what the HTTP serve mode provides.
	ModeMount Mode = "mount"

	// ModeAuto selects the best mode based on the source URI and availability
	// of FUSE in the container.
	ModeAuto Mode = "auto"
)

// Storage converts a remote asset URI into a form that ffmpeg can read without
// materialising the content to a local disk file.
type Storage interface {
	// Prepare returns a URL or path string that ffmpeg's -i flag can consume
	// directly, plus a cleanup function the caller must invoke when the asset
	// is no longer needed (kills the rclone subprocess, unmounts FUSE, etc.).
	//
	// sourceURI examples:
	//   s3://my-bucket/ref.yuv           — S3 via rclone remote "s3"
	//   gcs://bucket/path/dis.yuv        — GCS via rclone remote "gcs"
	//   rclone://s3-prod:bucket/ref.yuv  — explicit rclone remote:path syntax
	//   /local/path/to/ref.yuv           — bare local path (LocalStorage only)
	//   file:///local/path/to/ref.yuv    — file:// URI (LocalStorage only)
	Prepare(ctx context.Context, sourceURI string) (readableURL string, cleanup func(), err error)

	// Mode reports the active access strategy.
	Mode() Mode
}

// Config holds the configuration shared by all Storage implementations.
type Config struct {
	// RcloneBin is the path to the rclone binary. Defaults to "rclone".
	RcloneBin string

	// RcloneConfig is the path to the rclone configuration file.
	// Defaults to the value of VMAFX_RCLONE_CONFIG env var, then
	// /etc/vmafx/rclone.conf, then rclone's own default (~/.config/rclone/rclone.conf).
	RcloneConfig string

	// Mode selects the access strategy (http-serve | mount | auto).
	Mode Mode

	// Log receives diagnostic messages from the storage layer.
	Log *slog.Logger
}

// New returns the appropriate Storage implementation for the given config.
//
// Selection logic when Mode == ModeAuto:
//   - If the source URI looks like a local path (file:// or no scheme), use LocalStorage.
//   - If FUSE is unavailable (e.g., non-privileged distroless container), use HTTPServeStorage.
//   - Otherwise use HTTPServeStorage (default; avoids FUSE kernel dependency).
func New(cfg Config) Storage {
	log := cfg.Log
	if log == nil {
		log = slog.Default()
	}
	rcloneBin := cfg.RcloneBin
	if rcloneBin == "" {
		rcloneBin = "rclone"
	}

	mode := cfg.Mode
	if mode == ModeAuto || mode == "" {
		mode = ModeHTTPServe
	}

	switch mode {
	case ModeMount:
		return &FUSEMountStorage{
			rcloneBin:    rcloneBin,
			rcloneConfig: cfg.RcloneConfig,
			log:          log,
		}
	default:
		// ModeHTTPServe and ModeAuto both resolve to HTTPServeStorage.
		return &HTTPServeStorage{
			rcloneBin:    rcloneBin,
			rcloneConfig: cfg.RcloneConfig,
			log:          log,
		}
	}
}

// LocalStorage is a no-op Storage for assets that are already locally
// accessible (file:// URIs or bare paths). No rclone subprocess is spawned.
type LocalStorage struct{}

// Mode returns ModeAuto as a sentinel indicating no rclone is used.
func (s *LocalStorage) Mode() Mode { return ModeAuto }

// Prepare strips the file:// prefix if present and returns the bare path.
// The cleanup function is a no-op.
func (s *LocalStorage) Prepare(_ context.Context, sourceURI string) (string, func(), error) {
	path, err := localPath(sourceURI)
	if err != nil {
		return "", func() {}, err
	}
	return path, func() {}, nil
}

// IsLocal returns true if the URI is a local filesystem path (file:// or no scheme).
func IsLocal(sourceURI string) bool {
	if strings.HasPrefix(sourceURI, "/") || strings.HasPrefix(sourceURI, "./") {
		return true
	}
	u, err := url.Parse(sourceURI)
	if err != nil {
		return false
	}
	return u.Scheme == "" || u.Scheme == "file"
}

// localPath returns the filesystem path for a local URI.
func localPath(sourceURI string) (string, error) {
	if strings.HasPrefix(sourceURI, "/") || strings.HasPrefix(sourceURI, "./") {
		return sourceURI, nil
	}
	u, err := url.Parse(sourceURI)
	if err != nil {
		return "", fmt.Errorf("storage: parse URI %q: %w", sourceURI, err)
	}
	if u.Scheme == "" || u.Scheme == "file" {
		// file:///abs/path → /abs/path
		return u.Path, nil
	}
	return "", fmt.Errorf("storage: %q is not a local path", sourceURI)
}

// pickFreePort returns an available TCP port on localhost.
func pickFreePort() (int, error) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return 0, fmt.Errorf("storage: find free port: %w", err)
	}
	port := ln.Addr().(*net.TCPAddr).Port
	if closeErr := ln.Close(); closeErr != nil {
		return 0, fmt.Errorf("storage: close probe listener: %w", closeErr)
	}
	return port, nil
}

// rcloneRemotePath splits a sourceURI into the rclone remote:path form.
//
// Supported URI schemes:
//   - rclone://remote:bucket/path   → "remote:bucket/path"
//   - s3://bucket/key               → "s3:bucket/key" (rclone S3-remote convention)
//   - gcs://bucket/key              → "gcs:bucket/key"
//   - azblob://container/blob       → "azblob:container/blob"
//   - sftp://user@host:22/path      → "sftp:user@host:22/path"
//   - http://host/path              → passed through unchanged
//   - https://host/path             → passed through unchanged
//   - remote:path                   → passed through unchanged (already rclone syntax)
func rcloneRemotePath(sourceURI string) (string, error) {
	// Already in rclone remote:path form (no //).
	if !strings.Contains(sourceURI, "://") {
		return sourceURI, nil
	}

	// Special-case the rclone:// scheme before calling url.Parse because the
	// host component can contain a colon (e.g. "s3-prod:bucket"), which
	// url.Parse misinterprets as an invalid port number.
	if strings.HasPrefix(sourceURI, "rclone://") {
		rest := strings.TrimPrefix(sourceURI, "rclone://")
		// rest = "s3-prod:bucket/path" → pass through as-is.
		return rest, nil
	}

	u, err := url.Parse(sourceURI)
	if err != nil {
		return "", fmt.Errorf("storage: parse URI %q: %w", sourceURI, err)
	}

	switch u.Scheme {
	case "http", "https":
		// Already a URL ffmpeg can consume natively.
		return sourceURI, nil
	case "sftp", "ftp":
		// sftp://user@host:port/path
		// url.Parse puts "user@host:port" in Host and the userinfo separately in User.
		// rclone sftp syntax is "sftp:user@host:port/path" — reconstruct it.
		userHost := u.Host
		if u.User != nil && u.User.Username() != "" {
			userHost = u.User.Username() + "@" + u.Host
		}
		return u.Scheme + ":" + userHost + u.Path, nil
	default:
		// s3, gcs, azblob, b2, dropbox, onedrive, box, and any other rclone scheme:
		// map "scheme://bucket/key" → "scheme:bucket/key".
		return u.Scheme + ":" + u.Host + u.Path, nil
	}
}
