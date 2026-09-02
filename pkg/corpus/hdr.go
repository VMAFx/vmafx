// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/hdr.go — HDR handling for the corpus sweep.
//
// Detection and the per-codec flag dispatch are pkg/hdr's — the one Go port
// of vmaftune.hdr (ADR-0300, consolidated in ADR-1137) — re-exported here
// under the names the corpus package has always used. What stays local is
// corpus-specific: the existence check Python's detect_hdr performs before
// spawning ffprobe, the adaptation onto the RunResult-shaped Runner seam the
// rest of the package shares, and the HDR VMAF model resolver.

package corpus

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/VMAFx/vmafx/pkg/hdr"
)

// HdrInfo is the detected HDR signalling on a video stream: hdr.Info under
// the corpus package's name. Transfer is the canonical fork-local identifier
// ("pq" or "hlg"); Primaries and Matrix are the raw ffprobe strings.
type HdrInfo = hdr.Info

// DetectHDR probes videoPath and returns the HdrInfo, or nil for SDR sources,
// missing files, ffprobe failure, or any classification ambiguity.
//
// The missing-file check mirrors vmaftune.hdr.detect_hdr, which returns None
// before spawning ffprobe when the path does not exist; the probe and the
// classification are hdr.Detect's, driven through the corpus Runner seam.
func DetectHDR(ctx context.Context, videoPath, ffprobeBin string, run Runner) *HdrInfo {
	if _, err := os.Stat(videoPath); err != nil {
		return nil
	}
	runner := runnerOrExec(run)
	probe := func(ctx context.Context, argv []string) (string, int, error) {
		res := runner(ctx, argv)
		return res.Stdout, res.ReturnCode, nil
	}
	return hdr.Detect(ctx, videoPath, ffprobeBin, probe, nil)
}

// ClassifyFFprobePayload turns a decoded ffprobe JSON payload into an HdrInfo.
// Pure helper, exported so tests can pin the classification without a
// subprocess. The classifier is hdr.ClassifyPayload; re-encoding the decoded
// tree for it is lossless for an ffprobe document.
func ClassifyFFprobePayload(payload map[string]any) *HdrInfo {
	raw, err := json.Marshal(payload)
	if err != nil {
		return nil
	}
	return hdr.ClassifyPayload(raw, nil)
}

// HDRCodecArgs returns the ffmpeg argv tail that injects HDR signalling for
// encoder — hdr.CodecArgs under the corpus package's name.
//
// An empty result means the encoder has no HDR-specific flags or HDR is not
// yet wired for this codec adapter. Callers append the result after the -c:v
// argument. The dispatch table is the central contract: codec adapters own
// quality / preset argv, hdr owns colour signalling, so the corpus and the
// auto planner stay consistent across encoders.
func HDRCodecArgs(encoder string, info *HdrInfo) []string {
	return hdr.CodecArgs(encoder, info, nil)
}

// HDRModelFilename is the canonical HDR-model JSON name. It matches Netflix's
// research-artefact name so a future port is a verbatim file drop.
const HDRModelFilename = "vmaf_hdr_v0.6.1.json"

// HDRModelNameFor returns the HDR VMAF model filename for a transfer string.
// Any value other than "pq" / "hlg" (including "" for SDR) returns "" so the
// caller picks the SDR model. PQ and HLG share the model upstream —
// vmaf_hdr_v0.6.1 was trained on a mixed PQ + HLG corpus.
func HDRModelNameFor(transfer string) string {
	switch strings.ToLower(transfer) {
	case "pq", "hlg":
		return HDRModelFilename
	default:
		return ""
	}
}

// SelectHDRVMAFModel returns the path to the HDR-trained VMAF model JSON if one
// is shipped, else "".
//
// It prefers the canonical filename when transfer routing is requested, then
// falls back to the newest vmaf_hdr_*.json in modelDir so operators can ship
// revisions without breaking the resolver. Returning "" is the documented
// default: Netflix publishes vmaf_hdr_v0.6.1.json outside the upstream model/
// tree, and the fork awaits a licence review (ADR-0300 follow-up).
func SelectHDRVMAFModel(modelDir, transfer string) string {
	if modelDir == "" {
		modelDir = defaultModelDir()
	}
	if modelDir == "" {
		return ""
	}
	if info, err := os.Stat(modelDir); err != nil || !info.IsDir() {
		return ""
	}
	if canonical := HDRModelNameFor(transfer); canonical != "" {
		p := filepath.Join(modelDir, canonical)
		if info, err := os.Stat(p); err == nil && !info.IsDir() {
			return p
		}
	}
	matches, err := filepath.Glob(filepath.Join(modelDir, "vmaf_hdr_*.json"))
	if err != nil || len(matches) == 0 {
		return ""
	}
	sort.Strings(matches)
	return matches[len(matches)-1]
}

// ModelDir overrides the in-tree model/ directory lookup. Empty means "walk up
// from the working directory".
var ModelDir string

// defaultModelDir locates the in-tree model/ directory by walking up from the
// process working directory, mirroring the Python resolver's repo-root walk.
func defaultModelDir() string {
	if ModelDir != "" {
		return ModelDir
	}
	dir, err := os.Getwd()
	if err != nil {
		return ""
	}
	for {
		candidate := filepath.Join(dir, "model")
		if info, statErr := os.Stat(candidate); statErr == nil && info.IsDir() {
			if _, mErr := os.Stat(filepath.Join(candidate, "vmaf_v0.6.1.json")); mErr == nil {
				return candidate
			}
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return ""
		}
		dir = parent
	}
}
