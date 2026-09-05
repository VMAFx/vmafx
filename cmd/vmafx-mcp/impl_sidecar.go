// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

// impl_sidecar.go bridges the four sidecar CLI binaries that ship alongside
// the main `vmaf` tool into the MCP tool surface (epic #1240 item (b)):
//
//	vmaf-perShot  -> vmaf_per_shot   per-shot CRF plan (core/tools/vmaf_per_shot.c)
//	vmaf_roi      -> vmaf_roi        saliency ROI / qpfile sidecar (core/tools/vmaf_roi.c)
//	vmaf_bench    -> vmaf_bench      micro-benchmark + GPU validation (core/tools/vmaf_bench.c)
//	vmaf_vpl      -> vmaf_vpl        oneVPL zero-copy decode+score (core/tools/vmaf_vpl.c)
//
// Every handler follows the same contract as the score tools:
//
//   - every filesystem argument round-trips through libvmaf.ValidatePath /
//     libvmaf.ValidateDir (gosec G304/G204 contract, cmd/vmafx-mcp/AGENTS.md #7);
//   - every numeric / enum argument is bounds-checked against the exact ranges
//     the C parser enforces, so a bad value produces a clean MCP error instead
//     of a usage() dump on stderr;
//   - argv is built as a []string (never a shell string) and launched with
//     exec.CommandContext so a client disconnect kills the child (AGENTS.md #9);
//   - a non-zero exit becomes a returned error, which addRawTool turns into
//     isError=true;
//   - structured output (per-shot JSON plan, VPL score lines) is parsed into
//     the JSON response rather than handed back as an opaque blob.
//
// The Python server (mcp-server/vmaf-mcp/src/vmaf_mcp/server.py) carries the
// byte-identical twins of these handlers; see AGENTS.md invariant #15.

package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"regexp"
	"strconv"
	"strings"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// ---------------------------------------------------------------------------
// Shared sidecar helpers
// ---------------------------------------------------------------------------

// sidecarPixfmts / sidecarBitdepths mirror the enums accepted by
// vmaf_per_shot.c::per_shot_parse_pixfmt and vmaf_roi.c::parse_pixfmt.
var sidecarPixfmts = map[string]bool{"420": true, "422": true, "444": true}

func validSidecarBitdepth(b int) bool {
	return b == 8 || b == 10 || b == 12 || b == 16
}

// resolveSidecar returns the on-disk path of a sidecar binary, or an error
// naming the build command when it is absent. Kept in one place so every
// handler emits the same "build first" guidance.
func resolveSidecar(name string) (string, error) {
	bin := libvmaf.FindSidecarBinary(name)
	if bin == "" {
		return "", fmt.Errorf("unknown sidecar binary %q", name)
	}
	if _, err := os.Stat(bin); err != nil {
		return "", fmt.Errorf(
			"%s binary not found at %s. Build first: meson compile -C core/build %s "+
				"(or point %s at an existing build)",
			name, bin, name, libvmaf.SidecarBinaryEnv[name])
	}
	return bin, nil
}

// runSidecar executes argv and returns (stdout, stderr, exitCode, err). err is
// non-nil only when the process could not be started or was killed; a non-zero
// exit is reported through exitCode with err == nil so the caller decides
// whether that is a failure (it usually is) or a meaningful result
// (vmaf_bench --validate exits 1 when the GPU/CPU comparison finds deltas).
func runSidecar(ctx context.Context, argv []string) (string, string, int, error) {
	// #nosec G204 -- argv[0] comes from libvmaf.FindSidecarBinary (a fixed
	// candidate list plus a named env override, never a tool argument) and every
	// remaining element is either a literal flag or a value that has been
	// bounds-checked / libvmaf.ValidatePath-filtered by the calling handler.
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	err := cmd.Run()
	if err != nil {
		var exitErr *exec.ExitError
		if errors.As(err, &exitErr) {
			return stdout.String(), stderr.String(), exitErr.ExitCode(), nil
		}
		return stdout.String(), stderr.String(), -1,
			fmt.Errorf("%s could not be executed: %w", argv[0], err)
	}
	return stdout.String(), stderr.String(), 0, nil
}

// sidecarFailure builds the uniform error returned when a sidecar exits non-zero.
func sidecarFailure(name string, exitCode int, stdout, stderr string) error {
	detail := strings.TrimSpace(stderr)
	if detail == "" {
		detail = strings.TrimSpace(stdout)
	}
	if detail == "" {
		detail = "no output"
	}
	return fmt.Errorf("%s exited %d: %s", name, exitCode, detail)
}

// ---------------------------------------------------------------------------
// Tool: vmaf_per_shot  (core/tools/vmaf_per_shot.c)
// ---------------------------------------------------------------------------

// buildPerShotArgv validates args and returns (argv, format). Split out of the
// handler so the Go/Python argv-parity test can exercise it without a binary
// on disk (parity_argv_test.go / tests/test_sidecar_parity_1240.py).
func buildPerShotArgv(bin string, args map[string]any) ([]string, string, error) {
	ref, err := libvmaf.ValidatePath(strArg(args, "reference", ""))
	if err != nil {
		return nil, "", fmt.Errorf("reference: %w", err)
	}
	// per_shot_apply_opt bounds: width/height 16..65535.
	width := intArg(args, "width", 0)
	height := intArg(args, "height", 0)
	if width < 16 || width > 65535 || height < 16 || height > 65535 {
		return nil, "", fmt.Errorf("width and height must be between 16 and 65535")
	}
	pixfmt := strArg(args, "pixel_format", "420")
	if !sidecarPixfmts[pixfmt] {
		return nil, "", fmt.Errorf("invalid pixel_format %q: must be one of 420|422|444", pixfmt)
	}
	bitdepth := intArg(args, "bitdepth", 8)
	if !validSidecarBitdepth(bitdepth) {
		return nil, "", fmt.Errorf("invalid bitdepth %d: must be one of 8|10|12|16", bitdepth)
	}
	targetVmaf := floatArg(args, "target_vmaf", 90.0)
	if targetVmaf < 0 || targetVmaf > 100 {
		return nil, "", fmt.Errorf("target_vmaf must be between 0 and 100")
	}
	crfMin := intArg(args, "crf_min", 18)
	crfMax := intArg(args, "crf_max", 35)
	if crfMin < 0 || crfMin > 63 || crfMax < 0 || crfMax > 63 {
		return nil, "", fmt.Errorf("crf_min and crf_max must be between 0 and 63")
	}
	if crfMin > crfMax {
		return nil, "", fmt.Errorf("crf_min (%d) must not exceed crf_max (%d)", crfMin, crfMax)
	}
	format := strArg(args, "format", "json")
	if format != "json" && format != "csv" {
		return nil, "", fmt.Errorf("invalid format %q: must be json or csv", format)
	}

	argv := []string{
		bin,
		"--reference", ref,
		"--width", strconv.Itoa(width),
		"--height", strconv.Itoa(height),
		"--pixel_format", pixfmt,
		"--bitdepth", strconv.Itoa(bitdepth),
		"--output", "-",
		"--target-vmaf", strconv.FormatFloat(targetVmaf, 'f', -1, 64),
		"--crf-min", strconv.Itoa(crfMin),
		"--crf-max", strconv.Itoa(crfMax),
	}
	if hasArg(args, "diff_threshold") {
		dt := floatArg(args, "diff_threshold", 12.0)
		if dt < 0 || dt > 255 {
			return nil, "", fmt.Errorf("diff_threshold must be between 0 and 255")
		}
		argv = append(argv, "--diff-threshold", strconv.FormatFloat(dt, 'f', -1, 64))
	}
	argv = append(argv, "--format", format)
	return argv, format, nil
}

func handleVmafPerShot(ctx context.Context, args map[string]any) (any, error) {
	bin, err := resolveSidecar("vmaf-perShot")
	if err != nil {
		return nil, err
	}
	argv, format, err := buildPerShotArgv(bin, args)
	if err != nil {
		return nil, err
	}

	stdout, stderr, code, err := runSidecar(ctx, argv)
	if err != nil {
		return nil, err
	}
	if code != 0 {
		return nil, sidecarFailure("vmaf-perShot", code, stdout, stderr)
	}
	payload := map[string]any{
		"format":    format,
		"exit_code": code,
		"stderr":    stderr,
	}
	if format == "json" {
		var plan map[string]any
		if jsonErr := json.Unmarshal([]byte(stdout), &plan); jsonErr != nil {
			return nil, fmt.Errorf("failed to parse vmaf-perShot JSON plan: %w", jsonErr)
		}
		payload["plan"] = plan
	} else {
		payload["output"] = stdout
	}
	return payload, nil
}

// ---------------------------------------------------------------------------
// Tool: vmaf_roi  (core/tools/vmaf_roi.c)
// ---------------------------------------------------------------------------

// roiParams carries the validated values the response shape needs alongside argv.
type roiParams struct {
	encoder  string
	ctuSize  int
	frame    int
	width    int
	height   int
	saliency bool
}

// buildRoiArgv validates args and returns (argv, params). outPath is the
// caller-owned temp file the sidecar writes to. Split out of the handler so the
// Go/Python argv-parity test can exercise it without a binary on disk.
func buildRoiArgv(bin, outPath string, args map[string]any) ([]string, roiParams, error) {
	var p roiParams
	ref, err := libvmaf.ValidatePath(strArg(args, "reference", ""))
	if err != nil {
		return nil, p, fmt.Errorf("reference: %w", err)
	}
	// vmaf_roi.c: width/height 1..VMAF_ROI_MAX_DIM (16384), frame 0..1000000.
	width := intArg(args, "width", 0)
	height := intArg(args, "height", 0)
	if width < 1 || width > 16384 || height < 1 || height > 16384 {
		return nil, p, fmt.Errorf("width and height must be between 1 and 16384")
	}
	frame := intArg(args, "frame", -1)
	if frame < 0 || frame > 1000000 {
		return nil, p, fmt.Errorf("frame must be between 0 and 1000000")
	}
	pixfmt := strArg(args, "pixel_format", "420")
	if !sidecarPixfmts[pixfmt] {
		return nil, p, fmt.Errorf("invalid pixel_format %q: must be one of 420|422|444", pixfmt)
	}
	bitdepth := intArg(args, "bitdepth", 8)
	if !validSidecarBitdepth(bitdepth) {
		return nil, p, fmt.Errorf("invalid bitdepth %d: must be one of 8|10|12|16", bitdepth)
	}
	ctuSize := intArg(args, "ctu_size", 64)
	if ctuSize < 8 || ctuSize > 128 {
		return nil, p, fmt.Errorf("ctu_size must be between 8 and 128")
	}
	encoder := strArg(args, "encoder", "x265")
	if encoder != "x265" && encoder != "svt-av1" {
		return nil, p, fmt.Errorf("invalid encoder %q: must be x265 or svt-av1", encoder)
	}
	strength := floatArg(args, "strength", 6.0)
	if strength < 0 || strength > 64 {
		return nil, p, fmt.Errorf("strength must be between 0 and 64")
	}
	saliency := ""
	if raw := strArg(args, "saliency_model", ""); raw != "" {
		saliency, err = libvmaf.ValidatePath(raw)
		if err != nil {
			return nil, p, fmt.Errorf("saliency_model: %w", err)
		}
	}

	argv := []string{
		bin,
		"--reference", ref,
		"--width", strconv.Itoa(width),
		"--height", strconv.Itoa(height),
		"--frame", strconv.Itoa(frame),
		"--output", outPath,
		"--pixel_format", pixfmt,
		"--bitdepth", strconv.Itoa(bitdepth),
		"--ctu-size", strconv.Itoa(ctuSize),
		"--encoder", encoder,
		"--strength", strconv.FormatFloat(strength, 'f', -1, 64),
	}
	if saliency != "" {
		argv = append(argv, "--saliency-model", saliency)
	}
	p = roiParams{
		encoder:  encoder,
		ctuSize:  ctuSize,
		frame:    frame,
		width:    width,
		height:   height,
		saliency: saliency != "",
	}
	return argv, p, nil
}

func handleVmafRoi(ctx context.Context, args map[string]any) (any, error) {
	bin, err := resolveSidecar("vmaf_roi")
	if err != nil {
		return nil, err
	}
	// The svt-av1 emitter writes a raw int8 grid, so stdout is not text-safe.
	// Always route through a temp file and decide the response encoding from
	// the emitter, keeping both encoders on one code path.
	outFile, err := os.CreateTemp("", "vmaf-mcp-roi-*.bin")
	if err != nil {
		return nil, fmt.Errorf("failed to create temp output file: %w", err)
	}
	outPath := outFile.Name()
	if closeErr := outFile.Close(); closeErr != nil {
		return nil, fmt.Errorf("close temp output file: %w", closeErr)
	}
	defer func() {
		if rmErr := os.Remove(outPath); rmErr != nil && !errors.Is(rmErr, os.ErrNotExist) {
			fmt.Fprintf(os.Stderr, "handleVmafRoi: remove temp %s: %v\n", outPath, rmErr)
		}
	}()

	argv, p, err := buildRoiArgv(bin, outPath, args)
	if err != nil {
		return nil, err
	}

	stdout, stderr, code, err := runSidecar(ctx, argv)
	if err != nil {
		return nil, err
	}
	if code != 0 {
		return nil, sidecarFailure("vmaf-roi", code, stdout, stderr)
	}
	// #nosec G304 -- outPath is the os.CreateTemp name above, not caller input.
	data, err := os.ReadFile(outPath)
	if err != nil {
		return nil, fmt.Errorf("failed to read vmaf-roi sidecar: %w", err)
	}
	payload := map[string]any{
		"encoder":     p.encoder,
		"exit_code":   code,
		"stderr":      stderr,
		"ctu_size":    p.ctuSize,
		"frame":       p.frame,
		"bytes":       len(data),
		"grid_cols":   (p.width + p.ctuSize - 1) / p.ctuSize,
		"grid_rows":   (p.height + p.ctuSize - 1) / p.ctuSize,
		"saliency":    map[bool]string{true: "onnx", false: "placeholder"}[p.saliency],
		"sidecar_fmt": map[string]string{"x265": "qpfile", "svt-av1": "roi_map_int8"}[p.encoder],
	}
	if p.encoder == "x265" {
		payload["qpfile"] = string(data)
	} else {
		payload["roi_map_base64"] = base64.StdEncoding.EncodeToString(data)
	}
	return payload, nil
}

// ---------------------------------------------------------------------------
// Tool: vmaf_bench  (core/tools/vmaf_bench.c)
// ---------------------------------------------------------------------------

// benchResolutions mirrors the fixed table in vmaf_bench.c; --resolution only
// accepts one of these five WxH strings.
var benchResolutions = []string{"576x324", "640x480", "1280x720", "1920x1080", "3840x2160"}

// buildBenchArgv validates args and returns (argv, validateMode). Split out of
// the handler so the Go/Python argv-parity test can exercise it without a
// binary on disk.
func buildBenchArgv(bin string, args map[string]any) ([]string, bool, error) {
	argv := []string{bin}

	if hasArg(args, "frames") {
		frames := intArg(args, "frames", 10)
		// The C parser clamps <2 up to 2 and >48 (MAX_TEST_FRAMES) down to 48;
		// reject out-of-range values here so the caller gets a real error
		// instead of a silently different run.
		if frames < 2 || frames > 48 {
			return nil, false, fmt.Errorf("frames must be between 2 and 48 (MAX_TEST_FRAMES)")
		}
		argv = append(argv, "--frames", strconv.Itoa(frames))
	}
	if hasArg(args, "resolution") {
		res := strArg(args, "resolution", "")
		found := false
		for _, r := range benchResolutions {
			if r == res {
				found = true
				break
			}
		}
		if !found {
			return nil, false, fmt.Errorf("invalid resolution %q: must be one of %s",
				res, strings.Join(benchResolutions, ", "))
		}
		argv = append(argv, "--resolution", res)
	}
	if hasArg(args, "bpc") {
		bpc := intArg(args, "bpc", 8)
		if !validSidecarBitdepth(bpc) {
			return nil, false, fmt.Errorf("invalid bpc %d: must be one of 8|10|12|16", bpc)
		}
		argv = append(argv, "--bpc", strconv.Itoa(bpc))
	}
	if raw := strArg(args, "data_dir", ""); raw != "" {
		dir, dirErr := libvmaf.ValidateDir(raw)
		if dirErr != nil {
			return nil, false, fmt.Errorf("data_dir: %w", dirErr)
		}
		argv = append(argv, "--data-dir", dir)
	}
	if boolArg(args, "device_list", false) {
		argv = append(argv, "--list-devices")
	}
	if boolArg(args, "gpu_only", false) {
		argv = append(argv, "--gpu-only")
	}
	validate := boolArg(args, "validate", false)
	if validate {
		argv = append(argv, "--validate")
	}
	return argv, validate, nil
}

func handleVmafBench(ctx context.Context, args map[string]any) (any, error) {
	bin, err := resolveSidecar("vmaf_bench")
	if err != nil {
		return nil, err
	}
	argv, validate, err := buildBenchArgv(bin, args)
	if err != nil {
		return nil, err
	}

	stdout, stderr, code, err := runSidecar(ctx, argv)
	if err != nil {
		return nil, err
	}
	payload := map[string]any{
		"exit_code": code,
		"stdout":    stdout,
		"stderr":    stderr,
		"mode":      map[bool]string{true: "validate", false: "benchmark"}[validate],
	}
	// --validate exits 1 to report "some GPU/CPU comparison failed"; that is a
	// legitimate answer, not a tool error. Every other non-zero exit is.
	if validate {
		payload["validation_failed"] = code != 0
		return payload, nil
	}
	if code != 0 {
		return nil, sidecarFailure("vmaf_bench", code, stdout, stderr)
	}
	return payload, nil
}

// ---------------------------------------------------------------------------
// Tool: vmaf_vpl  (core/tools/vmaf_vpl.c)
// ---------------------------------------------------------------------------

// vplRenderNodeRe bounds --render-node to an actual DRM node. The flag is a
// device path handed straight to open(2); without this a caller could aim the
// sidecar at any file on the host.
var vplRenderNodeRe = regexp.MustCompile(`^/dev/dri/(renderD[0-9]+|card[0-9]+)$`)

// vplScoreRe / vplFramesRe parse the sidecar's human-readable summary lines
// ("VMAF:   96.123456 (mean)" / "Frames: 12") into structured fields.
var (
	vplScoreRe  = regexp.MustCompile(`(?m)^VMAF:\s+([0-9.eE+-]+)\s+\(mean\)`)
	vplFramesRe = regexp.MustCompile(`(?m)^Frames:\s+([0-9]+)`)
)

// vplParams carries the validated values the response shape echoes back.
type vplParams struct {
	model      string
	device     int
	renderNode string
}

// buildVplArgv validates args and returns (argv, params). Split out of the
// handler so the Go/Python argv-parity test can exercise it without a binary
// on disk (vmaf_vpl is only built when the oneVPL toolchain is present).
func buildVplArgv(bin string, args map[string]any) ([]string, vplParams, error) {
	var p vplParams
	ref, err := libvmaf.ValidatePath(strArg(args, "ref", ""))
	if err != nil {
		return nil, p, fmt.Errorf("ref: %w", err)
	}
	dis, err := libvmaf.ValidatePath(strArg(args, "dis", ""))
	if err != nil {
		return nil, p, fmt.Errorf("dis: %w", err)
	}
	model := strArg(args, "model", "vmaf_v0.6.1") // vmaf-model-pin: vmaf_vpl.c's own --model default
	if strings.ContainsAny(model, "/\\ \t") {
		return nil, p, fmt.Errorf("model must be a bare model name (e.g. vmaf_v0.6.1), not a path")
	}
	frames := intArg(args, "frames", 0)
	if frames < 0 {
		return nil, p, fmt.Errorf("frames must be >= 0 (0 = all frames)")
	}
	device := intArg(args, "device", 0)
	if device < 0 {
		return nil, p, fmt.Errorf("device must be >= 0")
	}
	renderNode := strArg(args, "render_node", "/dev/dri/renderD128")
	if !vplRenderNodeRe.MatchString(renderNode) {
		return nil, p, fmt.Errorf(
			"invalid render_node %q: must match /dev/dri/renderD<N> or /dev/dri/card<N>", renderNode)
	}

	argv := []string{
		bin,
		"--ref", ref,
		"--dis", dis,
		"--model", model,
		"--frames", strconv.Itoa(frames),
		"--device", strconv.Itoa(device),
		"--render-node", renderNode,
	}
	if boolArg(args, "fallback", false) {
		argv = append(argv, "--fallback")
	}
	p = vplParams{model: model, device: device, renderNode: renderNode}
	return argv, p, nil
}

func handleVmafVpl(ctx context.Context, args map[string]any) (any, error) {
	bin, err := resolveSidecar("vmaf_vpl")
	if err != nil {
		return nil, err
	}
	argv, p, err := buildVplArgv(bin, args)
	if err != nil {
		return nil, err
	}

	stdout, stderr, code, err := runSidecar(ctx, argv)
	if err != nil {
		return nil, err
	}
	if code != 0 {
		return nil, sidecarFailure("vmaf_vpl", code, stdout, stderr)
	}
	payload := map[string]any{
		"exit_code":   code,
		"stdout":      stdout,
		"stderr":      stderr,
		"model":       p.model,
		"device":      p.device,
		"render_node": p.renderNode,
	}
	if m := vplScoreRe.FindStringSubmatch(stdout); m != nil {
		if v, convErr := strconv.ParseFloat(m[1], 64); convErr == nil {
			payload["vmaf_score"] = v
		}
	}
	if m := vplFramesRe.FindStringSubmatch(stdout); m != nil {
		if v, convErr := strconv.Atoi(m[1]); convErr == nil {
			payload["frames_processed"] = v
		}
	}
	return payload, nil
}
