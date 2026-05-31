// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/encoder/discover_cache_test.go — regression coverage for the
// codec-cache stale-key bug.
//
// The original implementation used a one-shot sync.Once with the comment
// "Reset cache if bin changes (e.g. in tests)" followed by a literal
// "_ = ffmpegBin" no-op. The Once would fire on first call and lock in
// whichever binary path was probed first; subsequent calls with a
// different ffmpeg binary path silently returned the first-binary's
// codec set. In a dev-mcp / multi-container deployment this meant that
// the first call from a CPU-only ffmpeg "won" and h264_nvenc was
// reported "unavailable" forever — even after the caller switched to a
// CUDA-enabled binary path.

package encoder

import (
	"path/filepath"
	"testing"
)

func TestProbeAvailableCodecs_CacheRespectsBinaryPath(t *testing.T) {
	// Drop and restore the package-level cache so this test is independent
	// of any other test ordering.
	codecCacheMu.Lock()
	codecCacheMap = nil
	codecCacheBin = ""
	codecCacheMu.Unlock()
	t.Cleanup(func() {
		codecCacheMu.Lock()
		codecCacheMap = nil
		codecCacheBin = ""
		codecCacheMu.Unlock()
	})

	// Seed the cache as if "/path/to/ffmpeg-cpu" had been probed and
	// returned a set with libx264 only.
	codecCacheMu.Lock()
	codecCacheMap = map[string]bool{"libx264": true}
	codecCacheBin = "/path/to/ffmpeg-cpu"
	codecCacheMu.Unlock()

	// A second call with the SAME binary path must return the cached map.
	got := probeAvailableCodecs("/path/to/ffmpeg-cpu")
	if !got["libx264"] || got["h264_nvenc"] {
		t.Errorf("same-path cached probe = %v, want {libx264:true}", got)
	}

	// A call with a DIFFERENT binary path must trigger a re-probe. The
	// new binary path points at a non-existent file so runCodecProbe
	// returns an empty map (err != nil → empty map); the assertion is
	// that the cache is INVALIDATED — i.e. we no longer see libx264 from
	// the stale entry.
	cudaPath := filepath.Join(t.TempDir(), "definitely-no-such-ffmpeg")
	got2 := probeAvailableCodecs(cudaPath)
	if got2["libx264"] {
		t.Error("different-path probe still returned stale libx264 result; cache was not invalidated on binary-path change")
	}

	// Verify the cache now tracks the new binary path.
	codecCacheMu.Lock()
	gotBin := codecCacheBin
	codecCacheMu.Unlock()
	if gotBin != cudaPath {
		t.Errorf("cached binary path = %q, want %q (re-probe did not update cache key)", gotBin, cudaPath)
	}
}

func TestProbeAvailableCodecs_EmptyPathNormalisesToFfmpeg(t *testing.T) {
	codecCacheMu.Lock()
	codecCacheMap = nil
	codecCacheBin = ""
	codecCacheMu.Unlock()
	t.Cleanup(func() {
		codecCacheMu.Lock()
		codecCacheMap = nil
		codecCacheBin = ""
		codecCacheMu.Unlock()
	})

	// First call with "" must record the cache as "ffmpeg" so a follow-up
	// call with the literal "ffmpeg" hits the cache without re-probing.
	_ = probeAvailableCodecs("")
	codecCacheMu.Lock()
	got := codecCacheBin
	codecCacheMu.Unlock()
	if got != "ffmpeg" {
		t.Errorf("empty path normalisation: cache bin = %q, want %q", got, "ffmpeg")
	}
}

func TestRefreshCodecCache_ResetsBinAndMap(t *testing.T) {
	codecCacheMu.Lock()
	codecCacheMap = map[string]bool{"libx264": true}
	codecCacheBin = "/old/path"
	codecCacheMu.Unlock()
	t.Cleanup(func() {
		codecCacheMu.Lock()
		codecCacheMap = nil
		codecCacheBin = ""
		codecCacheMu.Unlock()
	})

	newPath := "/new/no-such-ffmpeg-binary-for-test"
	_ = RefreshCodecCache(newPath)
	codecCacheMu.Lock()
	gotBin := codecCacheBin
	codecCacheMu.Unlock()
	if gotBin != newPath {
		t.Errorf("RefreshCodecCache(%q): cache bin = %q, want %q", newPath, gotBin, newPath)
	}
}
