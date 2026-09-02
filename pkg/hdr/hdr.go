// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Package hdr is the Go port of vmaftune.hdr — HDR detection plus the
// codec-specific HDR encode-flag dispatch (ADR-0300).
//
// Detection is deliberately permissive: partial or missing colour metadata
// returns nil (the caller treats the source as SDR). Misclassifying SDR as
// HDR is the dangerous failure mode — it would inject PQ signalling into a
// gamma-2.4 encode. Misclassifying HDR as SDR is recoverable: the encode
// proceeds without HDR flags, scores trend low, and the operator re-runs.
package hdr

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"math"
	"os/exec"
	"strconv"
	"strings"
)

// Transfer-characteristic strings ffprobe emits. PQ = SMPTE ST 2084 (HDR10
// OETF); HLG = ARIB STD-B67 (broadcast HDR).
var pqTransfers = map[string]bool{
	"smpte2084": true, "smpte-st-2084": true, "smpte_st_2084": true,
}

var hlgTransfers = map[string]bool{
	"arib-std-b67": true, "arib_std_b67": true, "aribstdb67": true, "hlg": true,
}

// bt2020Primaries folds both non-constant (the common case) and constant
// luminance variants.
var bt2020Primaries = map[string]bool{
	"bt2020": true, "bt2020nc": true, "bt2020-ncl": true,
	"bt2020c": true, "bt2020-cl": true,
}

// Info is the detected HDR signalling on a video stream.
//
// Transfer is the canonical fork-local identifier ("pq" or "hlg"). Primaries
// and Matrix are the raw ffprobe strings — downstream encoders need them
// verbatim. MasterDisplay and MaxCLL are the SEI payload strings ffmpeg
// accepts, populated only when ffprobe surfaces them as stream side data.
type Info struct {
	Transfer      string
	Primaries     string
	Matrix        string
	ColorRange    string
	PixFmt        string
	MasterDisplay string
	MaxCLL        string
}

// DefaultForMetadataOnly returns the conservative BT.2020/PQ tuple used when
// a caller knows the source is HDR (SourceMeta.IsHDR) but has no probed Info
// — the metadata-only auto path and every test that injects a SourceMeta.
func DefaultForMetadataOnly() *Info {
	return &Info{
		Transfer:   "pq",
		Primaries:  "bt2020",
		Matrix:     "bt2020nc",
		ColorRange: "tv",
		PixFmt:     "yuv420p10le",
	}
}

// Runner is the subprocess seam. Production callers pass nil (CommandRunner
// is used); tests pass a stub returning a canned ffprobe JSON payload.
type Runner func(ctx context.Context, argv []string) (stdout string, exitCode int, err error)

// CommandRunner executes argv and captures stdout. A non-zero exit is
// reported through exitCode, not err — err is reserved for spawn failures, so
// callers can distinguish "ffprobe said no" from "ffprobe is not installed".
func CommandRunner(ctx context.Context, argv []string) (string, int, error) {
	if len(argv) == 0 {
		return "", 1, fmt.Errorf("hdr: empty argv")
	}
	// #nosec G204 -- argv[0] is an operator-configured binary name and the
	// tail is fixed ffprobe flags plus a CLI-supplied source path; this is a
	// dev-time tool, not an RPC surface.
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	out, err := cmd.Output()
	if err == nil {
		return string(out), 0, nil
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		return string(out), exitErr.ExitCode(), nil
	}
	return "", 1, err
}

// Detect probes videoPath and returns the HDR signalling, or nil for SDR
// sources, ffprobe failure, or any classification ambiguity.
func Detect(ctx context.Context, videoPath, ffprobeBin string, run Runner, log *slog.Logger) *Info {
	if log == nil {
		log = slog.Default()
	}
	if ffprobeBin == "" {
		ffprobeBin = "ffprobe"
	}
	if run == nil {
		run = CommandRunner
	}
	argv := []string{
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
	stdout, code, err := run(ctx, argv)
	if err != nil {
		log.Debug("hdr-detect: ffprobe invocation failed", "error", err)
		return nil
	}
	if code != 0 {
		log.Debug("hdr-detect: ffprobe returned non-zero", "exit_code", code)
		return nil
	}
	return ClassifyPayload([]byte(stdout), log)
}

// ffprobeStream is the subset of the ffprobe -show_streams payload Detect
// consumes.
type ffprobeStream struct {
	ColorTransfer  string           `json:"color_transfer"`
	ColorPrimaries string           `json:"color_primaries"`
	ColorSpace     string           `json:"color_space"`
	ColorRange     string           `json:"color_range"`
	PixFmt         string           `json:"pix_fmt"`
	SideDataList   []map[string]any `json:"side_data_list"`
}

// ClassifyPayload turns a raw ffprobe JSON payload into an Info, or nil.
// Exported so tests can pin the classification without a subprocess.
func ClassifyPayload(payload []byte, log *slog.Logger) *Info {
	if log == nil {
		log = slog.Default()
	}
	var doc struct {
		Streams []ffprobeStream `json:"streams"`
	}
	if err := json.Unmarshal(payload, &doc); err != nil {
		return nil
	}
	if len(doc.Streams) == 0 {
		return nil
	}
	s := doc.Streams[0]

	transfer := strings.ToLower(s.ColorTransfer)
	primaries := strings.ToLower(s.ColorPrimaries)
	matrix := strings.ToLower(s.ColorSpace)
	colorRange := strings.ToLower(s.ColorRange)
	pixFmt := strings.ToLower(s.PixFmt)

	var canonical string
	switch {
	case pqTransfers[transfer]:
		canonical = "pq"
	case hlgTransfers[transfer]:
		canonical = "hlg"
	default:
		return nil
	}

	// PQ/HLG transfer without BT.2020 primaries is malformed; treat as SDR
	// rather than injecting mismatched signalling.
	if !bt2020Primaries[primaries] {
		log.Warn("hdr-detect: HDR transfer with non-bt2020 primaries; treating as SDR",
			"transfer", canonical, "primaries", primaries)
		return nil
	}

	masterDisplay, maxCLL := extractMastering(s.SideDataList)
	info := &Info{
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

func orDefault(v, fallback string) string {
	if v == "" {
		return fallback
	}
	return v
}

// extractMastering pulls the mastering-display and content-light SEI payloads
// out of ffprobe side data, formatted the way x265 / SVT-AV1 expect.
// Chromaticity is in 0.00002 units, luminance in 0.0001 cd/m².
func extractMastering(sideData []map[string]any) (masterDisplay, maxCLL string) {
	for _, sd := range sideData {
		kind := strings.ToLower(asString(sd["side_data_type"]))
		switch {
		case strings.Contains(kind, "mastering display"):
			masterDisplay = formatMasterDisplay(sd)
		case strings.Contains(kind, "content light"):
			mc, mcOK := sd["max_content"]
			ma, maOK := sd["max_average"]
			if mcOK && maOK && mc != nil && ma != nil {
				// Python formats these with a bare int() — no 0.0001-unit
				// scaling, unlike the mastering-display coordinates.
				maxCLL = fmt.Sprintf("%d,%d", truncInt(mc), truncInt(ma))
			}
		}
	}
	return masterDisplay, maxCLL
}

// formatMasterDisplay renders the x265 "master-display" string
// G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min). Returns "" when any coordinate is
// absent — a partial payload is worse than none.
func formatMasterDisplay(sd map[string]any) string {
	keys := []string{
		"green_x", "green_y", "blue_x", "blue_y",
		"red_x", "red_y", "white_point_x", "white_point_y",
	}
	coords := make([]int64, 0, len(keys))
	for _, k := range keys {
		v, ok := sd[k]
		if !ok || v == nil {
			return ""
		}
		coords = append(coords, fracToUnit(v, 50000))
	}
	lmax, lmaxOK := sd["max_luminance"]
	lmin, lminOK := sd["min_luminance"]
	if !lmaxOK || !lminOK || lmax == nil || lmin == nil {
		return ""
	}
	return fmt.Sprintf("G(%d,%d)B(%d,%d)R(%d,%d)WP(%d,%d)L(%d,%d)",
		coords[0], coords[1], coords[2], coords[3],
		coords[4], coords[5], coords[6], coords[7],
		fracToUnit(lmax, 10000), fracToUnit(lmin, 10000))
}

// fracToUnit converts ffprobe's "num/den" fraction (or a bare number) to a
// scaled integer. Unparseable input yields 0, matching the Python helper.
func fracToUnit(value any, scale int64) int64 {
	switch v := value.(type) {
	case float64:
		return int64(math.Round(v * float64(scale)))
	case int:
		return int64(math.Round(float64(v) * float64(scale)))
	case string:
		if num, den, ok := strings.Cut(v, "/"); ok {
			n, nErr := strconv.ParseFloat(strings.TrimSpace(num), 64)
			d, dErr := strconv.ParseFloat(strings.TrimSpace(den), 64)
			if nErr != nil || dErr != nil || d == 0 {
				return 0
			}
			return int64(math.Round(n / d * float64(scale)))
		}
		f, err := strconv.ParseFloat(strings.TrimSpace(v), 64)
		if err != nil {
			return 0
		}
		return int64(math.Round(f * float64(scale)))
	default:
		return 0
	}
}

// truncInt mirrors CPython's int(x) on a JSON scalar: truncate a float
// toward zero, parse a decimal string, 0 for anything else.
func truncInt(value any) int64 {
	switch v := value.(type) {
	case float64:
		return int64(v)
	case int:
		return int64(v)
	case string:
		n, err := strconv.ParseInt(strings.TrimSpace(v), 10, 64)
		if err != nil {
			return 0
		}
		return n
	default:
		return 0
	}
}

func asString(v any) string {
	if s, ok := v.(string); ok {
		return s
	}
	return ""
}

// ---------------------------------------------------------------------------
// Codec-side flag dispatch.
// ---------------------------------------------------------------------------

// CodecArgs returns the ffmpeg argv tail that injects HDR signalling for
// encoder. An empty slice means the encoder has no HDR-specific flags or HDR
// is not yet wired for that adapter; callers append the result after -c:v.
//
// The dispatch table is the central contract: codec adapters own the
// quality/preset argv, this function owns HDR colour signalling, so the
// corpus and the auto planner stay consistent across encoders.
func CodecArgs(encoder string, info *Info, log *slog.Logger) []string {
	if info == nil {
		return nil
	}
	if log == nil {
		log = slog.Default()
	}
	switch encoder {
	case "libaom-av1":
		return globalColorArgs(info)
	case "libx264":
		// x264 cannot represent HDR in-stream; emit the container-level
		// colour tags so the metadata survives the mux anyway.
		return globalColorArgs(info)
	case "libx265":
		return x265Args(info)
	case "libsvtav1":
		return svtAV1Args(info)
	case "av1_nvenc", "av1_qsv", "av1_amf":
		return av1TenBitGlobal(info)
	case "hevc_nvenc":
		return nvencHEVCArgs(info)
	case "hevc_qsv", "hevc_amf", "hevc_videotoolbox":
		return hevcMain10Global(info)
	case "libvvenc":
		return globalColorArgs(info)
	default:
		log.Warn("hdr-codec-args: no HDR dispatch for encoder; emitting empty",
			"encoder", encoder)
		return nil
	}
}

// globalColorArgs are the ffmpeg-level -color_* flags every HDR-capable codec
// wants.
func globalColorArgs(info *Info) []string {
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

// hevcMain10Global is the HEVC hardware-family baseline: force 10-bit output
// plus the global colour tags.
func hevcMain10Global(info *Info) []string {
	return append([]string{"-pix_fmt", "p010le", "-profile:v", "main10"},
		globalColorArgs(info)...)
}

// av1TenBitGlobal is the AV1 hardware-family baseline: 10-bit 4:2:0 plus the
// global colour tags.
func av1TenBitGlobal(info *Info) []string {
	return append([]string{"-pix_fmt", "p010le"}, globalColorArgs(info)...)
}

// x265Args emits in-stream SEI through -x265-params.
func x265Args(info *Info) []string {
	transfer := "arib-std-b67"
	if info.Transfer == "pq" {
		transfer = "smpte2084"
	}
	rangeArg := "range=limited"
	if info.ColorRange == "pc" {
		rangeArg = "range=full"
	}
	parts := []string{
		"colorprim=bt2020",
		"transfer=" + transfer,
		"colormatrix=bt2020nc",
		rangeArg,
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

// svtAV1Args emits -svtav1-params. AV1 enums: 9 = BT.2020,
// 16 = SMPTE-2084 (PQ), 18 = ARIB-STD-B67 (HLG).
func svtAV1Args(info *Info) []string {
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

// nvencHEVCArgs relies on 10-bit main10 plus the ffmpeg-global colour flags
// for SEI propagation, with the explicit SEI knobs when available.
func nvencHEVCArgs(info *Info) []string {
	args := hevcMain10Global(info)
	if info.MasterDisplay != "" {
		args = append(args, "-master_display", info.MasterDisplay)
	}
	if info.MaxCLL != "" {
		args = append(args, "-max_cll", info.MaxCLL)
	}
	return args
}
