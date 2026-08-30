// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/hdr.go — Go port of vmaftune.hdr (ADR-0300).
//
// HDR sources carry distinct colour metadata (BT.2020 primaries, PQ /
// SMPTE-2084 or HLG / ARIB STD-B67 transfer) and codecs expect that metadata
// back on the encode side through codec-specific flag families. This file:
//
//  1. probes a video with "ffprobe -show_streams -of json" and classifies the
//     first video stream as PQ HDR / HLG HDR / SDR;
//  2. emits the codec-appropriate ffmpeg flag list per detected adapter;
//  3. resolves a fork-local HDR VMAF model JSON if one is shipped, else returns
//     "" so callers fall back to the SDR model with a logged warning.
//
// Detection is deliberately permissive: partial / missing colour metadata reads
// as SDR. Misclassifying SDR as HDR is the dangerous failure mode (it would
// inject PQ flags into a gamma-2.4 encode); misclassifying HDR as SDR is
// recoverable (scores trend low, the user re-runs with --force-hdr-*).

package corpus

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// Transfer-characteristic strings ffprobe emits. PQ = SMPTE ST 2084
// (Dolby / HDR10 OETF); HLG = ARIB STD-B67 (BBC / NHK broadcast HDR).
var (
	pqTransfers  = map[string]bool{"smpte2084": true, "smpte-st-2084": true, "smpte_st_2084": true}
	hlgTransfers = map[string]bool{
		"arib-std-b67": true, "arib_std_b67": true, "aribstdb67": true, "hlg": true,
	}
	// bt2020Primaries folds both ncl (the common case) and cl.
	bt2020Primaries = map[string]bool{
		"bt2020": true, "bt2020nc": true, "bt2020-ncl": true,
		"bt2020c": true, "bt2020-cl": true,
	}
)

// HdrInfo is the detected HDR signalling on a video stream.
//
// Transfer is the canonical fork-local identifier ("pq" or "hlg"). Primaries
// and Matrix are the raw ffprobe strings; downstream encoders need them
// verbatim. MasterDisplay and MaxCLL are the SEI-payload strings ffmpeg
// accepts, populated only when ffprobe surfaces them via stream side-data.
type HdrInfo struct {
	Transfer      string
	Primaries     string
	Matrix        string
	ColorRange    string
	PixFmt        string
	MasterDisplay string
	MaxCLL        string
}

// DetectHDR probes videoPath and returns the HdrInfo, or nil for SDR sources,
// missing files, ffprobe failure, or any classification ambiguity.
func DetectHDR(ctx context.Context, videoPath, ffprobeBin string, run Runner) *HdrInfo {
	if _, err := os.Stat(videoPath); err != nil {
		return nil
	}
	if ffprobeBin == "" {
		ffprobeBin = "ffprobe"
	}
	cmd := []string{
		ffprobeBin,
		"-v", "error",
		"-select_streams", "v:0",
		"-show_streams",
		"-show_entries",
		"stream=color_transfer,color_primaries,color_space,color_range,pix_fmt:" +
			"stream_side_data=side_data_type,red_x,red_y,green_x,green_y,blue_x,blue_y," +
			"white_point_x,white_point_y,min_luminance,max_luminance,max_content,max_average",
		"-of", "json",
		videoPath,
	}
	res := runnerOrExec(run)(ctx, cmd)
	if res.ReturnCode != 0 {
		return nil
	}
	var payload map[string]any
	if err := json.Unmarshal([]byte(res.Stdout), &payload); err != nil {
		return nil
	}
	return ClassifyFFprobePayload(payload)
}

// ClassifyFFprobePayload turns a decoded ffprobe JSON payload into an HdrInfo.
// Pure helper, exported so tests can pin the classification without a
// subprocess.
func ClassifyFFprobePayload(payload map[string]any) *HdrInfo {
	streamsRaw, ok := payload["streams"].([]any)
	if !ok || len(streamsRaw) == 0 {
		return nil
	}
	s, ok := streamsRaw[0].(map[string]any)
	if !ok {
		return nil
	}

	transfer := lowerString(s["color_transfer"])
	primaries := lowerString(s["color_primaries"])
	matrix := lowerString(s["color_space"])
	colorRange := lowerString(s["color_range"])
	pixFmt := lowerString(s["pix_fmt"])

	var canonical string
	switch {
	case pqTransfers[transfer]:
		canonical = "pq"
	case hlgTransfers[transfer]:
		canonical = "hlg"
	default:
		return nil
	}

	// PQ / HLG transfer without BT.2020 primaries is malformed; treat it as
	// SDR so we do not inject mismatched signalling. Users with edge-case
	// sources bypass via --force-hdr-*.
	if !bt2020Primaries[primaries] {
		slog.Warn("hdr-detect: HDR transfer with non-bt2020 primaries; treating as SDR",
			"transfer", canonical, "primaries", primaries)
		return nil
	}

	masterDisplay, maxCLL := extractMastering(s["side_data_list"])
	info := &HdrInfo{
		Transfer:      canonical,
		Primaries:     primaries,
		Matrix:        orDefault(matrix, "bt2020nc"),
		ColorRange:    orDefault(colorRange, "tv"),
		PixFmt:        orDefault(pixFmt, "yuv420p10le"),
		MasterDisplay: masterDisplay,
		MaxCLL:        maxCLL,
	}
	return info
}

func lowerString(v any) string {
	s, ok := v.(string)
	if !ok {
		return ""
	}
	return strings.ToLower(s)
}

func orDefault(v, def string) string {
	if v == "" {
		return def
	}
	return v
}

// extractMastering pulls the mastering-display and content-light SEI payloads
// out of ffprobe side data, in the format x265 / SVT-AV1 expect.
func extractMastering(sideDataRaw any) (string, string) {
	list, ok := sideDataRaw.([]any)
	if !ok {
		return "", ""
	}
	var md, cll string
	for _, entry := range list {
		sd, ok := entry.(map[string]any)
		if !ok {
			continue
		}
		kind := lowerString(sd["side_data_type"])
		switch {
		case strings.Contains(kind, "mastering display"):
			md = formatMasterDisplay(sd)
		case strings.Contains(kind, "content light"):
			mc, mcOK := fracToUnit(sd["max_content"], 1)
			ma, maOK := fracToUnit(sd["max_average"], 1)
			if mcOK && maOK {
				cll = fmt.Sprintf("%d,%d", mc, ma)
			}
		}
	}
	return md, cll
}

// formatMasterDisplay renders the x265 "master-display" string
// G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min).
//
// ffprobe surfaces the coordinates as "<num>/<den>" fraction strings; encoders
// expect chroma in 0.00002 units and luminance in 0.0001 cd/m² units, which is
// what the 50000 / 10000 scale factors produce.
func formatMasterDisplay(sd map[string]any) string {
	keys := []string{
		"green_x", "green_y", "blue_x", "blue_y",
		"red_x", "red_y", "white_point_x", "white_point_y",
	}
	coords := make([]int, 0, len(keys))
	for _, k := range keys {
		v, ok := sd[k]
		if !ok || v == nil {
			return ""
		}
		scaled, ok := fracToUnit(v, 50000)
		if !ok {
			// Python's _frac_to_unit returns 0 for unparseable input
			// rather than bailing; mirror that.
			scaled = 0
		}
		coords = append(coords, scaled)
	}
	lmaxRaw, hasMax := sd["max_luminance"]
	lminRaw, hasMin := sd["min_luminance"]
	if !hasMax || !hasMin || lmaxRaw == nil || lminRaw == nil {
		return ""
	}
	lmax, _ := fracToUnit(lmaxRaw, 10000)
	lmin, _ := fracToUnit(lminRaw, 10000)
	return fmt.Sprintf("G(%d,%d)B(%d,%d)R(%d,%d)WP(%d,%d)L(%d,%d)",
		coords[0], coords[1], coords[2], coords[3],
		coords[4], coords[5], coords[6], coords[7], lmax, lmin)
}

// fracToUnit converts ffprobe's "num/den" fraction (or a plain number) to a
// scaled integer. ok is false only when the value is nil.
func fracToUnit(value any, scale int) (int, bool) {
	switch t := value.(type) {
	case nil:
		return 0, false
	case float64:
		return int(math.Round(t * float64(scale))), true
	case int:
		return t * scale, true
	}
	text := fmt.Sprintf("%v", value)
	if num, den, found := strings.Cut(text, "/"); found {
		n, nErr := strconv.ParseFloat(num, 64)
		d, dErr := strconv.ParseFloat(den, 64)
		if nErr != nil || dErr != nil || d == 0 {
			return 0, true
		}
		return int(math.Round((n / d) * float64(scale))), true
	}
	f, err := strconv.ParseFloat(text, 64)
	if err != nil {
		return 0, true
	}
	return int(math.Round(f * float64(scale))), true
}

// HDRCodecArgs returns the ffmpeg argv tail that injects HDR signalling for
// encoder.
//
// An empty result means the encoder has no HDR-specific flags or HDR is not yet
// wired for this codec adapter. Callers append the result after the -c:v
// argument. The dispatch table is the central contract: codec adapters own
// quality / preset argv, this function owns HDR colour signalling so the corpus
// and auto planner stay consistent across encoders.
func HDRCodecArgs(encoder string, info *HdrInfo) []string {
	if info == nil {
		return nil
	}
	switch encoder {
	case "libaom-av1":
		return globalColorArgs(info)
	case "libx264":
		// x264 has no in-stream HDR signalling beyond container-level
		// colour tags; emit them so HDR sources keep their metadata in
		// the muxed file. Real HDR encodes should use x265 / SVT-AV1.
		return globalColorArgs(info)
	case "libx265":
		return hdrArgsX265(info)
	case "libsvtav1":
		return hdrArgsSVTAV1(info)
	case "av1_nvenc", "av1_qsv", "av1_amf":
		return hdrArgsAV110BitGlobal(info)
	case "hevc_nvenc":
		return hdrArgsNVENCHEVC(info)
	case "hevc_qsv", "hevc_amf", "hevc_videotoolbox":
		return hdrArgsHEVCMain10Global(info)
	case "libvvenc":
		// VVenC uses the same -color_* shape as x265 for the global
		// metadata; SEI options live behind --vvenc-params in newer
		// ffmpeg builds.
		return globalColorArgs(info)
	default:
		slog.Warn("hdr-codec-args: no HDR dispatch for encoder; emitting empty",
			"encoder", encoder)
		return nil
	}
}

// globalColorArgs are the ffmpeg-level -color_* flags every HDR-capable codec
// wants.
func globalColorArgs(info *HdrInfo) []string {
	transfer := "arib-std-b67"
	if info.Transfer == "pq" {
		transfer = "smpte2084"
	}
	return []string{
		"-color_primaries", "bt2020",
		"-color_trc", transfer,
		"-colorspace", orDefault(info.Matrix, "bt2020nc"),
		"-color_range", orDefault(info.ColorRange, "tv"),
	}
}

// hdrArgsHEVCMain10Global is the HEVC hardware-family baseline: force 10-bit
// output plus the global colour tags.
func hdrArgsHEVCMain10Global(info *HdrInfo) []string {
	args := []string{"-pix_fmt", "p010le", "-profile:v", "main10"}
	return append(args, globalColorArgs(info)...)
}

// hdrArgsAV110BitGlobal is the AV1 hardware-family baseline: force 10-bit 4:2:0
// output plus the global colour tags.
func hdrArgsAV110BitGlobal(info *HdrInfo) []string {
	args := []string{"-pix_fmt", "p010le"}
	return append(args, globalColorArgs(info)...)
}

// hdrArgsX265 adds in-stream SEI via -x265-params.
func hdrArgsX265(info *HdrInfo) []string {
	transfer := "arib-std-b67"
	if info.Transfer == "pq" {
		transfer = "smpte2084"
	}
	colorRange := "range=limited"
	if info.ColorRange == "pc" {
		colorRange = "range=full"
	}
	parts := []string{
		"colorprim=bt2020",
		"transfer=" + transfer,
		"colormatrix=bt2020nc",
		colorRange,
	}
	if info.MasterDisplay != "" {
		parts = append(parts, "master-display="+info.MasterDisplay)
	}
	if info.MaxCLL != "" {
		parts = append(parts, "max-cll="+info.MaxCLL)
	}
	if info.Transfer == "pq" {
		parts = append(parts, "hdr10-opt=1")
	}
	return append(globalColorArgs(info), "-x265-params", strings.Join(parts, ":"))
}

// hdrArgsSVTAV1 adds -svtav1-params colour signalling.
//
// AV1 enum: 9 = BT.2020, 16 = SMPTE-2084 (PQ), 18 = ARIB-STD-B67 (HLG).
func hdrArgsSVTAV1(info *HdrInfo) []string {
	tc := 18
	if info.Transfer == "pq" {
		tc = 16
	}
	colorRange := "color-range=0"
	if info.ColorRange == "pc" {
		colorRange = "color-range=1"
	}
	parts := []string{
		"color-primaries=9",
		fmt.Sprintf("transfer-characteristics=%d", tc),
		"matrix-coefficients=9",
		colorRange,
	}
	if info.MasterDisplay != "" {
		parts = append(parts, "mastering-display="+info.MasterDisplay)
	}
	if info.MaxCLL != "" {
		parts = append(parts, "content-light="+info.MaxCLL)
	}
	return append(globalColorArgs(info), "-svtav1-params", strings.Join(parts, ":"))
}

// hdrArgsNVENCHEVC relies on -pix_fmt p010le -profile:v main10 plus the global
// -color_* flags for SEI propagation, with the explicit NVENC SEI knobs when
// the payloads are known.
func hdrArgsNVENCHEVC(info *HdrInfo) []string {
	args := hdrArgsHEVCMain10Global(info)
	if info.MasterDisplay != "" {
		args = append(args, "-master_display", info.MasterDisplay)
	}
	if info.MaxCLL != "" {
		args = append(args, "-max_cll", info.MaxCLL)
	}
	return args
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
