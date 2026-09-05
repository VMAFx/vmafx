// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

// impl.go contains the handler implementations for each of the 15 MCP tools.
// Each handler delegates to the vmaf or vmaf-tune CLI binary, matching the
// behaviour of the Python server (mcp-server/vmaf-mcp/src/vmaf_mcp/server.py).

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/modeleval"
)

// ---------------------------------------------------------------------------
// Backend-disable map — mirrors the Python _BACKEND_DISABLE dict.
// ---------------------------------------------------------------------------

var backendDisable = map[string][]string{
	"cpu":   {"cuda", "sycl", "hip", "metal"},
	"cuda":  {"sycl", "hip", "metal"},
	"sycl":  {"cuda", "hip", "metal"},
	"hip":   {"cuda", "sycl", "metal"},
	"metal": {"cuda", "sycl", "hip"},
}

// ---------------------------------------------------------------------------
// Backend probe cache — populated lazily, mirrors _BACKEND_PROBE_CACHE.
// ---------------------------------------------------------------------------

var (
	probeMu    sync.Mutex
	probeCache = map[string]map[string]bool{}
)

// probeBackends returns the set of backends the vmaf binary advertises
// (via --help flags). "cpu" is always included.
func probeBackends(vmafBin string) map[string]bool {
	probeMu.Lock()
	defer probeMu.Unlock()

	if cached, ok := probeCache[vmafBin]; ok {
		return cached
	}

	advertised := map[string]bool{"cpu": true}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	out, err := exec.CommandContext(ctx, vmafBin, "--help").CombinedOutput()
	if err == nil {
		blob := string(out)
		for _, name := range []string{"cuda", "sycl", "hip", "metal"} {
			if strings.Contains(blob, "--no_"+name) {
				advertised[name] = true
			}
		}
	}
	probeCache[vmafBin] = advertised
	return advertised
}

// ---------------------------------------------------------------------------
// String arg helpers
// ---------------------------------------------------------------------------

func strArg(args map[string]any, key, def string) string {
	if v, ok := args[key]; ok {
		switch s := v.(type) {
		case string:
			return s
		default:
			return fmt.Sprintf("%v", v)
		}
	}
	return def
}

func intArg(args map[string]any, key string, def int) int {
	if v, ok := args[key]; ok {
		switch n := v.(type) {
		case float64:
			return int(n)
		case int:
			return n
		case json.Number:
			// Return def (not 0) on parse failure so callers don't silently
			// pass --width 0 / --bitdepth 0 to the vmaf subprocess.
			i, err := n.Int64()
			if err != nil {
				return def
			}
			return int(i)
		}
	}
	return def
}

func floatArg(args map[string]any, key string, def float64) float64 {
	if v, ok := args[key]; ok {
		switch n := v.(type) {
		case float64:
			return n
		case json.Number:
			// Return def (not 0) on parse failure so callers don't silently
			// pass malformed numeric args to the vmaf subprocess.
			f, err := n.Float64()
			if err != nil {
				return def
			}
			return f
		}
	}
	return def
}

func boolArg(args map[string]any, key string, def bool) bool {
	if v, ok := args[key]; ok {
		if b, ok := v.(bool); ok {
			return b
		}
	}
	return def
}

func hasArg(args map[string]any, key string) bool {
	_, ok := args[key]
	return ok
}

// ---------------------------------------------------------------------------
// Optional scoring pass-through flags (ADR-1117)
//
// scoreExtras carries the optional tiny-AI/DNN, feature-selection, CTC-preset,
// and frame-range parameters shared by vmaf_score + vmaf_score_encoded. Each
// field maps onto a `vmaf` CLI flag verified against core/tools/cli_parse.c.
// A zero-value scoreExtras adds no flags (backward-compatible).
//
// This MUST stay byte-compatible with the Python server's equivalent argv
// construction in `_run_vmaf_score` (server.py) — same flags, same values.
// ---------------------------------------------------------------------------

var (
	validPixfmts = map[string]bool{
		"420": true,
		"422": true,
		"444": true,
	}
	validBackends = map[string]bool{
		"auto":  true,
		"cpu":   true,
		"cuda":  true,
		"sycl":  true,
		"hip":   true,
		"metal": true,
	}
	validOutputFmts = map[string]bool{
		"json": true,
		"xml":  true,
		"csv":  true,
		"sub":  true,
	}
	validTinyDevices = map[string]bool{
		"auto":         true,
		"cpu":          true,
		"cuda":         true,
		"openvino":     true,
		"openvino-npu": true,
		"openvino-cpu": true,
		"openvino-gpu": true,
		"coreml":       true,
		"coreml-ane":   true,
		"coreml-gpu":   true,
		"coreml-cpu":   true,
		"rocm":         true,
	}
	validTinyResizes = map[string]bool{
		"bilinear": true,
		"nearest":  true,
		"bicubic":  true,
		"disabled": true,
	}
	validAOMCTCs = map[string]bool{
		"v1.0": true,
		"v2.0": true,
		"v3.0": true,
		"v4.0": true,
		"v5.0": true,
		"v6.0": true,
		"v7.0": true,
	}
	validNFLXCTCs = map[string]bool{
		"v1.0": true,
	}
)

type scoreExtras struct {
	features        []string // repeated --feature
	aomCTC          string   // --aom_ctc
	nflxCTC         string   // --nflx_ctc
	tinyModel       string   // --tiny-model
	tinyDevice      string   // --tiny-device (alias --dnn-ep)
	tinyThreads     *int     // --tiny-threads
	tinyFP16        bool     // --tiny-fp16
	tinyModelVerify bool     // --tiny-model-verify
	tinyCodec       string   // --tiny-codec
	tinyPreset      string   // --tiny-preset
	tinyCRF         *int     // --tiny-crf
	tinyResize      string   // --tiny-resize
	noReference     bool     // --no-reference
	threads         *int     // --threads
	frameCnt        *int     // --frame_cnt
	frameSkipRef    *int     // --frame_skip_ref
	frameSkipDist   *int     // --frame_skip_dist
	noPrediction    bool     // --no_prediction
	subsample       int      // --subsample (emitted only when > 1; vmaf_score_encoded)
	cpumask         *int     // --cpumask
	gpumask         *int     // --gpumask
	syclDevice      *int     // --sycl_device
	hipDevice       *int     // --hip_device
	metalDevice     *int     // --metal_device
	outputFmt       string   // --json | --xml | --csv | --sub
	disableClip     bool     // :disable_clip on model
	enableTransform bool     // :enable_transform on model
}

// isZero reports whether no scoring-extra flag is set, i.e. the request is a
// plain FR score that the cgo direct path can still serve (ADR-1117 / ADR-0931).
func (ex scoreExtras) isZero() bool {
	return len(ex.features) == 0 && ex.aomCTC == "" && ex.nflxCTC == "" &&
		ex.tinyModel == "" && ex.tinyDevice == "" && ex.tinyThreads == nil &&
		!ex.tinyFP16 && !ex.tinyModelVerify && ex.tinyCodec == "" &&
		ex.tinyPreset == "" && ex.tinyCRF == nil && ex.tinyResize == "" &&
		!ex.noReference && ex.threads == nil && ex.frameCnt == nil &&
		ex.frameSkipRef == nil && ex.frameSkipDist == nil && !ex.noPrediction &&
		ex.subsample <= 1 &&
		ex.cpumask == nil && ex.gpumask == nil && ex.syclDevice == nil &&
		ex.hipDevice == nil && ex.metalDevice == nil &&
		!ex.disableClip && !ex.enableTransform &&
		(ex.outputFmt == "" || ex.outputFmt == "json")
}

// optIntArg returns a pointer to the int value at key, or nil when the key is
// absent. Used so the handler can distinguish "unset" (omit the flag) from an
// explicit 0 (which is a valid value for e.g. --tiny-threads / --frame_skip_*).
func optIntArg(args map[string]any, key string) *int {
	if !hasArg(args, key) {
		return nil
	}
	v := intArg(args, key, 0)
	return &v
}


// parseScoreExtras extracts and validates the optional scoring pass-through flags from args.
func parseScoreExtras(args map[string]any) (scoreExtras, error) {
	tinyDevice := strArg(args, "tiny_device", "")
	dnnEP := strArg(args, "dnn_ep", "")
	if tinyDevice != "" && dnnEP != "" && tinyDevice != dnnEP {
		return scoreExtras{}, fmt.Errorf("conflicting tiny_device (%q) and dnn_ep (%q) values", tinyDevice, dnnEP)
	}
	if tinyDevice == "" {
		tinyDevice = dnnEP
	}
	if tinyDevice != "" && !validTinyDevices[tinyDevice] {
		return scoreExtras{}, fmt.Errorf("invalid tiny_device %q: must be one of auto|cpu|cuda|openvino|openvino-npu|openvino-cpu|openvino-gpu|coreml|coreml-ane|coreml-gpu|coreml-cpu|rocm", tinyDevice)
	}

	tinyResize := strArg(args, "tiny_resize", "")
	if tinyResize != "" && !validTinyResizes[tinyResize] {
		return scoreExtras{}, fmt.Errorf("invalid tiny_resize %q: must be one of bilinear|nearest|bicubic|disabled", tinyResize)
	}

	tinyCRF := optIntArg(args, "tiny_crf")
	if tinyCRF != nil && (*tinyCRF < 0 || *tinyCRF > 63) {
		return scoreExtras{}, fmt.Errorf("invalid tiny_crf %d: must be in range [0, 63]", *tinyCRF)
	}

	tinyThreads := optIntArg(args, "tiny_threads")
	if tinyThreads != nil && *tinyThreads < 0 {
		return scoreExtras{}, fmt.Errorf("invalid tiny_threads %d: must be >= 0", *tinyThreads)
	}

	aomCTC := strArg(args, "aom_ctc", "")
	if aomCTC != "" && !validAOMCTCs[aomCTC] {
		return scoreExtras{}, fmt.Errorf("invalid aom_ctc %q: must be one of v1.0|v2.0|v3.0|v4.0|v5.0|v6.0|v7.0", aomCTC)
	}

	nflxCTC := strArg(args, "nflx_ctc", "")
	if nflxCTC != "" && !validNFLXCTCs[nflxCTC] {
		return scoreExtras{}, fmt.Errorf("invalid nflx_ctc %q: must be v1.0", nflxCTC)
	}

	threads := optIntArg(args, "threads")
	if threads != nil && *threads < 1 {
		return scoreExtras{}, fmt.Errorf("invalid threads %d: must be >= 1", *threads)
	}

	frameCnt := optIntArg(args, "frame_cnt")
	if frameCnt != nil && *frameCnt < 1 {
		return scoreExtras{}, fmt.Errorf("invalid frame_cnt %d: must be >= 1", *frameCnt)
	}

	frameSkipRef := optIntArg(args, "frame_skip_ref")
	if frameSkipRef != nil && *frameSkipRef < 0 {
		return scoreExtras{}, fmt.Errorf("invalid frame_skip_ref %d: must be non-negative", *frameSkipRef)
	}

	frameSkipDist := optIntArg(args, "frame_skip_dist")
	if frameSkipDist != nil && *frameSkipDist < 0 {
		return scoreExtras{}, fmt.Errorf("invalid frame_skip_dist %d: must be non-negative", *frameSkipDist)
	}

	subsample := intArg(args, "subsample", 1)
	if subsample < 1 {
		return scoreExtras{}, fmt.Errorf("invalid subsample %d: must be >= 1", subsample)
	}

	cpumask := optIntArg(args, "cpumask")
	if cpumask != nil && *cpumask < 0 {
		return scoreExtras{}, fmt.Errorf("invalid cpumask %d: must be non-negative", *cpumask)
	}

	gpumask := optIntArg(args, "gpumask")
	if gpumask != nil && *gpumask < 0 {
		return scoreExtras{}, fmt.Errorf("invalid gpumask %d: must be non-negative", *gpumask)
	}

	syclDevice := optIntArg(args, "sycl_device")
	if syclDevice != nil && *syclDevice < 0 {
		return scoreExtras{}, fmt.Errorf("invalid sycl_device %d: must be non-negative", *syclDevice)
	}

	hipDevice := optIntArg(args, "hip_device")
	if hipDevice != nil && *hipDevice < 0 {
		return scoreExtras{}, fmt.Errorf("invalid hip_device %d: must be non-negative", *hipDevice)
	}

	metalDevice := optIntArg(args, "metal_device")
	if metalDevice != nil && *metalDevice < 0 {
		return scoreExtras{}, fmt.Errorf("invalid metal_device %d: must be non-negative", *metalDevice)
	}

	csvFlag := boolArg(args, "csv", false)
	subFlag := boolArg(args, "sub", false)
	if csvFlag && subFlag {
		return scoreExtras{}, fmt.Errorf("conflicting csv and sub flags: cannot specify both")
	}
	rawFmt := strArg(args, "output_fmt", "")
	if rawFmt == "" {
		rawFmt = strArg(args, "format", "")
	}
	outputFmt := rawFmt
	if csvFlag {
		if rawFmt != "" && rawFmt != "csv" {
			return scoreExtras{}, fmt.Errorf("conflicting output format: csv flag cannot be used with output_fmt %q", rawFmt)
		}
		outputFmt = "csv"
	} else if subFlag {
		if rawFmt != "" && rawFmt != "sub" {
			return scoreExtras{}, fmt.Errorf("conflicting output format: sub flag cannot be used with output_fmt %q", rawFmt)
		}
		outputFmt = "sub"
	} else if outputFmt == "" {
		outputFmt = "json"
	}
	if !validOutputFmts[outputFmt] {
		return scoreExtras{}, fmt.Errorf("invalid output_fmt %q: must be one of json|xml|csv|sub", outputFmt)
	}

	ex := scoreExtras{
		aomCTC:          aomCTC,
		nflxCTC:         nflxCTC,
		tinyModel:       strArg(args, "tiny_model", ""),
		tinyDevice:      tinyDevice,
		tinyThreads:     tinyThreads,
		tinyFP16:        boolArg(args, "tiny_fp16", false),
		tinyModelVerify: boolArg(args, "tiny_model_verify", false),
		tinyCodec:       strArg(args, "tiny_codec", ""),
		tinyPreset:      strArg(args, "tiny_preset", ""),
		tinyCRF:         tinyCRF,
		tinyResize:      tinyResize,
		noReference:     boolArg(args, "no_reference", false),
		threads:         threads,
		frameCnt:        frameCnt,
		frameSkipRef:    frameSkipRef,
		frameSkipDist:   frameSkipDist,
		noPrediction:    boolArg(args, "no_prediction", false),
		subsample:       subsample,
		cpumask:         cpumask,
		gpumask:         gpumask,
		syclDevice:      syclDevice,
		hipDevice:       hipDevice,
		metalDevice:     metalDevice,
		outputFmt:       outputFmt,
		disableClip:     boolArg(args, "disable_clip", false),
		enableTransform: boolArg(args, "enable_transform", false),
	}

	if raw, ok := args["feature"].([]any); ok {
		for _, f := range raw {
			if s, isStr := f.(string); isStr && s != "" {
				ex.features = append(ex.features, s)
			}
		}
	}
	return ex, nil
}

// appendArgs appends every set scoring-extra flag to argv in a fixed,
// deterministic order (matches the Python server's argv build).
func (ex scoreExtras) appendArgs(argv []string) []string {
	// --subsample is emitted first (right after --json in runVmafScore) so the
	// argv order matches the Python server, and only when > 1 (parity with
	// `if req.subsample > 1` in server.py). Fixes a pre-existing parity bug
	// where vmaf_score_encoded declared subsample but never forwarded it.
	if ex.subsample > 1 {
		argv = append(argv, "--subsample", strconv.Itoa(ex.subsample))
	}
	for _, f := range ex.features {
		argv = append(argv, "--feature", f)
	}
	if ex.aomCTC != "" {
		argv = append(argv, "--aom_ctc", ex.aomCTC)
	}
	if ex.nflxCTC != "" {
		argv = append(argv, "--nflx_ctc", ex.nflxCTC)
	}
	if ex.tinyModel != "" {
		argv = append(argv, "--tiny-model", ex.tinyModel)
	}
	if ex.tinyDevice != "" {
		argv = append(argv, "--tiny-device", ex.tinyDevice)
	}
	if ex.tinyThreads != nil {
		argv = append(argv, "--tiny-threads", strconv.Itoa(*ex.tinyThreads))
	}
	if ex.tinyFP16 {
		argv = append(argv, "--tiny-fp16")
	}
	if ex.tinyModelVerify {
		argv = append(argv, "--tiny-model-verify")
	}
	if ex.tinyCodec != "" {
		argv = append(argv, "--tiny-codec", ex.tinyCodec)
	}
	if ex.tinyPreset != "" {
		argv = append(argv, "--tiny-preset", ex.tinyPreset)
	}
	if ex.tinyCRF != nil {
		argv = append(argv, "--tiny-crf", strconv.Itoa(*ex.tinyCRF))
	}
	if ex.tinyResize != "" {
		argv = append(argv, "--tiny-resize", ex.tinyResize)
	}
	if ex.noReference {
		argv = append(argv, "--no-reference")
	}
	if ex.threads != nil {
		argv = append(argv, "--threads", strconv.Itoa(*ex.threads))
	}
	if ex.frameCnt != nil {
		argv = append(argv, "--frame_cnt", strconv.Itoa(*ex.frameCnt))
	}
	if ex.frameSkipRef != nil {
		argv = append(argv, "--frame_skip_ref", strconv.Itoa(*ex.frameSkipRef))
	}
	if ex.frameSkipDist != nil {
		argv = append(argv, "--frame_skip_dist", strconv.Itoa(*ex.frameSkipDist))
	}
	if ex.noPrediction {
		argv = append(argv, "--no_prediction")
	}
	if ex.cpumask != nil {
		argv = append(argv, "--cpumask", strconv.Itoa(*ex.cpumask))
	}
	if ex.gpumask != nil {
		argv = append(argv, "--gpumask", strconv.Itoa(*ex.gpumask))
	}
	if ex.syclDevice != nil {
		argv = append(argv, "--sycl_device", strconv.Itoa(*ex.syclDevice))
	}
	if ex.hipDevice != nil {
		argv = append(argv, "--hip_device", strconv.Itoa(*ex.hipDevice))
	}
	if ex.metalDevice != nil {
		argv = append(argv, "--metal_device", strconv.Itoa(*ex.metalDevice))
	}
	return argv
}

// ---------------------------------------------------------------------------
// Tool: vmaf_score
// ---------------------------------------------------------------------------

func handleVmafScore(ctx context.Context, args map[string]any) (any, error) {
	extras, err := parseScoreExtras(args)
	if err != nil {
		return nil, err
	}
	// NR-mode flag/model consistency check (mirrors cli_parse.c:997): the only
	// scorer that runs without a reference is an NR tiny ONNX model, so
	// --no-reference requires --tiny-model. Surface the same error the CLI
	// would, rather than letting the subprocess exit with a usage() dump.
	if extras.noReference && extras.tinyModel == "" {
		return nil, fmt.Errorf("no_reference requires tiny_model; no classic NR scorer exists")
	}
	// --- ref/dis path handling ---
	// In NR mode the reference is not consumed by the scorer, but the CLI still
	// requires a -r path to satisfy its reader; the caller may still supply one.
	// We validate ref only when present (NR) or always (FR).
	refRaw := strArg(args, "ref", "")
	var ref string
	if refRaw != "" {
		ref, err = libvmaf.ValidatePath(refRaw)
		if err != nil {
			return nil, fmt.Errorf("ref: %w", err)
		}
	} else if !extras.noReference {
		return nil, fmt.Errorf("missing required argument: 'ref' (omit only with no_reference=true)")
	}
	dis, err := libvmaf.ValidatePath(strArg(args, "dis", ""))
	if err != nil {
		return nil, fmt.Errorf("dis: %w", err)
	}
	width := intArg(args, "width", 0)
	height := intArg(args, "height", 0)
	if width <= 0 || height <= 0 {
		return nil, fmt.Errorf("width and height must be positive integers")
	}
	pixfmt := strArg(args, "pixfmt", "420")
	if !validPixfmts[pixfmt] {
		return nil, fmt.Errorf("invalid pixfmt %q: must be one of 420|422|444", pixfmt)
	}
	bitdepth := intArg(args, "bitdepth", 8)
	if bitdepth != 8 && bitdepth != 10 && bitdepth != 12 && bitdepth != 16 {
		return nil, fmt.Errorf("invalid bitdepth %d: must be one of 8|10|12|16", bitdepth)
	}
	backend := strArg(args, "backend", "auto")
	if !validBackends[backend] {
		return nil, fmt.Errorf("invalid backend %q: must be one of auto|cpu|cuda|sycl|hip|metal", backend)
	}
	model := strArg(args, "model", "version=vmaf_v0.6.1")
	precision := strArg(args, "precision", "legacy") // "legacy"=%.6f matches C CLI default (ADR-0119)

	// When VMAFX_MCP_DIRECT=1, attempt the direct cgo path first.  The cgo
	// path falls back to the subprocess path transparently for backends /
	// model types it cannot handle (ADR-0931 Phase 1). The direct path does
	// not yet support the extended scoring flags, so route through the
	// subprocess path whenever any extra flag is set (ADR-1117).
	if directPathEnabled() && extras.isZero() {
		return runVmafScoreDirect(ctx, ref, dis, width, height, pixfmt, bitdepth, model, backend)
	}
	return runVmafScore(ctx, ref, dis, width, height, pixfmt, bitdepth, model, backend, precision, extras)
}

// buildVmafArgv constructs the complete argv list for invoking the vmaf CLI,
// maintaining strict byte-parity with the Python vmaf-mcp server.
func buildVmafArgv(vmafBin, ref, dis string, width, height int, pixfmt string, bitdepth int, model, backend, precision, outPath string, extras scoreExtras) []string {
	if extras.disableClip && !strings.Contains(model, ":disable_clip") {
		model += ":disable_clip"
	}
	if extras.enableTransform && !strings.Contains(model, ":enable_transform") {
		model += ":enable_transform"
	}
	argv := []string{}
	if vmafBin != "" {
		argv = append(argv, vmafBin)
	}
	// In no-reference mode the reference path may be omitted (only the
	// distorted picture is scored by the NR tiny model). When a ref is still
	// supplied — FR mode, or an NR caller passing one — emit -r as usual.
	if ref != "" {
		argv = append(argv, "-r", ref)
	}
	argv = append(argv,
		"-d", dis,
		"--width", strconv.Itoa(width),
		"--height", strconv.Itoa(height),
		"-p", pixfmt,
		"-b", strconv.Itoa(bitdepth),
		"-m", model,
		"--precision", precision,
		"-q",
		"-o", outPath,
	)
	switch extras.outputFmt {
	case "xml":
		argv = append(argv, "--xml")
	case "csv":
		argv = append(argv, "--csv")
	case "sub":
		argv = append(argv, "--sub")
	default:
		argv = append(argv, "--json")
	}
	argv = extras.appendArgs(argv)
	if siblings, ok := backendDisable[backend]; ok {
		for _, s := range siblings {
			argv = append(argv, "--no_"+s)
		}
	}
	return argv
}

func runVmafScore(ctx context.Context, ref, dis string, width, height int, pixfmt string, bitdepth int, model, backend, precision string, extras scoreExtras) (map[string]any, error) {
	vmafBin := libvmaf.FindBinary()
	if _, err := os.Stat(vmafBin); err != nil {
		return nil, fmt.Errorf("vmaf binary not found at %s. Build first: meson compile -C build", vmafBin)
	}

	if backend != "auto" {
		advertised := probeBackends(vmafBin)
		if !advertised[backend] {
			return nil, fmt.Errorf(
				"backend %q requested but the local vmaf binary does not advertise it; "+
					"refusing to fall back silently. Pass backend='auto' to let vmaf pick",
				backend,
			)
		}
	}

	tempPattern := fmt.Sprintf("vmaf-mcp-*.%s", extras.outputFmt)
	outFile, err := os.CreateTemp("", tempPattern)
	if err != nil {
		return nil, fmt.Errorf("failed to create temp output file: %w", err)
	}
	outPath := outFile.Name()
	if closeErr := outFile.Close(); closeErr != nil {
		return nil, fmt.Errorf("close temp output file: %w", closeErr)
	}
	defer func() {
		if rmErr := os.Remove(outPath); rmErr != nil && !errors.Is(rmErr, os.ErrNotExist) {
			// Best-effort cleanup; surface via stderr but don't fail the call.
			fmt.Fprintf(os.Stderr, "runVmafScore: remove temp %s: %v\n", outPath, rmErr)
		}
	}()

	argv := buildVmafArgv(vmafBin, ref, dis, width, height, pixfmt, bitdepth, model, backend, precision, outPath, extras)

	// #nosec G204 -- vmafBin resolved via libvmaf.FindBinary (env-overridable to
	// a fixed allowlist of paths) and `ref`/`dis` are already libvmaf.ValidatePath-
	// filtered in the calling handlers (handleVmafScore + handleVmafScoreEncoded).
	// CommandContext propagates client-disconnect cancellation so that the vmaf
	// subprocess is killed when the MCP client disconnects mid-run rather than
	// running to completion as an orphan (ADR-1085).
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return nil, fmt.Errorf("vmaf exited %v: %s", err, stderr.String())
	}

	// #nosec G304 -- outPath is the value returned by os.CreateTemp above, not
	// a caller-supplied path; it cannot escape /tmp.
	data, err := os.ReadFile(outPath)
	if err != nil {
		return nil, fmt.Errorf("failed to read vmaf output: %w", err)
	}
	if extras.outputFmt != "" && extras.outputFmt != "json" {
		return map[string]any{
			"format":            extras.outputFmt,
			"output":            string(data),
			"backend_requested": backend,
			"backend_used":      backend,
		}, nil
	}
	var payload map[string]any
	if err := json.Unmarshal(data, &payload); err != nil {
		return nil, fmt.Errorf("failed to parse vmaf output: %w", err)
	}
	payload["backend_requested"] = backend
	if backend != "auto" {
		payload["backend_used"] = backend
	} else {
		payload["backend_used"] = inferBackendFromPayload(payload)
	}
	// Resolution mismatch warning (mirrors Python Bug #5 fix).
	if w := resolutionMismatchWarning(model, width, height); w != "" {
		payload["mismatched_model_warning"] = w
	}
	return payload, nil
}

// inferBackendFromPayload guesses the backend from the frame metrics count.
// Mirrors the Python _infer_backend_from_payload for byte-parity:
//   - 0 frames → cpu (safe default)
//   - <= 12 metrics → gpu (generic GPU path; stripped metric set)
//   - > 12 metrics → cpu  (standard CPU feature set)
//
// (ADR-0726 removed the Vulkan backend; the prior >=30→"vulkan" branch
// emitted a value the Python side never produces — a parity violation.)
func inferBackendFromPayload(payload map[string]any) string {
	frames, _ := payload["frames"].([]any)
	if len(frames) == 0 {
		return "cpu"
	}
	first, _ := frames[0].(map[string]any)
	metrics, _ := first["metrics"].(map[string]any)
	n := len(metrics)
	if n <= 12 {
		return "gpu"
	}
	return "cpu"
}

// Model resolution hints — mirrors Python _MODEL_RES_HINTS.
var modelResHints = []struct{ needle, class string }{
	{"vmaf_4k", "4k"},
	{"vmaf_v0.6.1neg", "hd"},
	{"vmaf_v0.6.1", "hd"},
	{"vmaf_b_v0.6.3", "hd"},
}

func classifySourceResolution(width, height int) string {
	long := width
	if height > long {
		long = height
	}
	if long >= 3840 {
		return "4k"
	}
	if long >= 1280 {
		return "hd"
	}
	return "sd"
}

func modelResolutionClass(model string) string {
	lower := strings.ToLower(model)
	for _, h := range modelResHints {
		if strings.Contains(lower, h.needle) {
			return h.class
		}
	}
	return ""
}

func resolutionMismatchWarning(model string, width, height int) string {
	mc := modelResolutionClass(model)
	if mc == "" {
		return ""
	}
	sc := classifySourceResolution(width, height)
	if mc == sc {
		return ""
	}
	return fmt.Sprintf(
		"model resolution preset %q does not match source %dx%d (%q); "+
			"scores may saturate or be biased — pick a model trained at the matching resolution.",
		mc, width, height, sc,
	)
}

// ---------------------------------------------------------------------------
// Tool: list_models
// ---------------------------------------------------------------------------

func handleListModels(_ context.Context, _ map[string]any) (any, error) {
	root := libvmaf.RepoRoot()
	modelsDir := filepath.Join(root, "model")
	var models []map[string]any

	err := filepath.WalkDir(modelsDir, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil //nolint:nilerr // skip unreadable dirs
		}
		if d.IsDir() {
			return nil
		}
		ext := strings.ToLower(filepath.Ext(path))
		if ext != ".json" && ext != ".pkl" && ext != ".onnx" {
			return nil
		}
		fi, err := d.Info()
		if err != nil {
			return nil //nolint:nilerr
		}
		rel, _ := filepath.Rel(root, path)
		stem := strings.TrimSuffix(filepath.Base(path), filepath.Ext(path))
		models = append(models, map[string]any{
			"name":       stem,
			"path":       rel,
			"format":     strings.TrimPrefix(ext, "."),
			"size_bytes": fi.Size(),
		})
		return nil
	})
	if err != nil {
		return nil, fmt.Errorf("failed to enumerate models: %w", err)
	}
	// Sort by path for deterministic output (matches Python sorted()).
	sort.Slice(models, func(i, j int) bool {
		return models[i]["path"].(string) < models[j]["path"].(string)
	})
	return map[string]any{"models": models}, nil
}

// ---------------------------------------------------------------------------
// Tool: list_backends
// ---------------------------------------------------------------------------

func handleListBackends(_ context.Context, _ map[string]any) (any, error) {
	vmafBin := libvmaf.FindBinary()
	if _, err := os.Stat(vmafBin); err != nil {
		return map[string]bool{
			"cpu": true, "cuda": false, "sycl": false,
			"hip": false, "metal": false,
		}, nil
	}
	advertised := probeBackends(vmafBin)
	return map[string]bool{
		"cpu":   true,
		"cuda":  advertised["cuda"],
		"sycl":  advertised["sycl"],
		"hip":   advertised["hip"],
		"metal": advertised["metal"],
	}, nil
}

// ---------------------------------------------------------------------------
// Tool: run_benchmark
// ---------------------------------------------------------------------------

func handleRunBenchmark(ctx context.Context, _ map[string]any) (any, error) {
	root := libvmaf.RepoRoot()
	script := filepath.Join(root, "testdata", "bench_all.sh")
	if _, err := os.Stat(script); err != nil {
		return nil, fmt.Errorf("benchmark harness not found: %s", script)
	}

	// Resolve vmaf_root: prefer env, then repo root if fixture exists, then /workspace.
	fixturePart := filepath.Join("python", "test", "resource", "yuv", "src01_hrc00_576x324.yuv")
	vmafRoot := root
	if env := os.Getenv("VMAF_ROOT"); env != "" {
		vmafRoot = env
	} else {
		if _, err := os.Stat(filepath.Join(root, fixturePart)); err != nil {
			if _, err2 := os.Stat(filepath.Join("/workspace", fixturePart)); err2 == nil {
				vmafRoot = "/workspace"
			}
		}
	}

	env := append(os.Environ(),
		"VMAF_ROOT="+vmafRoot,
		"VMAF_BIN="+libvmaf.FindBinary(),
	)
	// #nosec G204 -- script path is constructed from libvmaf.RepoRoot() and a
	// fixed relative path (testdata/bench_all.sh); not caller-controlled.
	cmd := exec.CommandContext(ctx, "bash", script)
	cmd.Env = env
	out, err := cmd.CombinedOutput()
	// Split stdout/stderr is not available from CombinedOutput, but the Python
	// server's callers only use the combined output anyway. We put it all in stdout.
	payload := map[string]any{
		"exit_code": 0,
		"stdout":    string(out),
		"stderr":    "",
	}
	if err != nil {
		if exitErr, ok := err.(*exec.ExitError); ok {
			payload["exit_code"] = exitErr.ExitCode()
		} else {
			payload["exit_code"] = -1
		}
		if strings.TrimSpace(string(out)) == "" {
			payload["error"] = fmt.Sprintf(
				"bench_all.sh exited with no output — likely aborted by set -euo pipefail. "+
					"Missing vmaf binary at %s or fixture YUVs?", libvmaf.FindBinary(),
			)
		}
	}
	return payload, nil
}

// ---------------------------------------------------------------------------
// Tool: eval_model_on_split
// ---------------------------------------------------------------------------

// handleEvalModelOnSplit evaluates an ONNX tiny-AI regressor against a
// parquet feature cache and reports PLCC / SROCC / RMSE.
//
// This is a fully native implementation. It previously shelled out to
// python3 with an inline script that imported numpy + pandas + scipy +
// onnxruntime; that subprocess was the last hard Python dependency in
// the Go MCP server and the blocker for the ADR-0704 Stage 2 sunset of
// mcp-server/vmaf-mcp. The pieces now live in:
//
//   - pkg/modeleval — parquet read, split bucketing, statistics (pure Go)
//   - pkg/libvmaf   — the ONNX forward pass, via libvmaf's own ONNX
//     Runtime session API (vmaf_dnn_session_*), so no third-party ONNX
//     package enters the Go module
//
// When libvmaf was built without ONNX Runtime every session call returns
// -ENOSYS and the caller sees ErrDNNUnavailable — the same graceful
// degradation the Python server shows when its optional `eval` extra is
// not installed.
func handleEvalModelOnSplit(ctx context.Context, args map[string]any) (any, error) {
	modelPath, err := libvmaf.ValidatePath(strArg(args, "model", ""))
	if err != nil {
		return nil, fmt.Errorf("model: %w", err)
	}
	featuresPath, err := libvmaf.ValidatePath(strArg(args, "features", ""))
	if err != nil {
		return nil, fmt.Errorf("features: %w", err)
	}
	opts := modeleval.Options{
		ModelPath:    modelPath,
		FeaturesPath: featuresPath,
		Split:        strArg(args, "split", modeleval.SplitTest),
		InputName:    strArg(args, "input_name", modeleval.DefaultInputName),
	}

	// Validate the split before opening an ORT session so a typo costs
	// nothing, matching the Python ordering.
	if err := modeleval.ValidateSplit(opts.Split); err != nil {
		return nil, err
	}

	pred, closeFn, err := newDNNPredictor(modelPath)
	if err != nil {
		return nil, err
	}
	defer closeFn()

	return modeleval.Evaluate(ctx, pred, opts)
}

// newDNNPredictor opens a libvmaf ONNX session for modelPath and adapts
// it to modeleval.Predictor. The returned func always releases the
// session — the Python original leaked one ORT session per evaluated
// model (docs/research/0983-python-surfaces-bug-audit-2026-05-31.md).
func newDNNPredictor(modelPath string) (modeleval.Predictor, func(), error) {
	sess, err := libvmaf.OpenDNNSession(modelPath)
	if err != nil {
		return nil, func() {}, err
	}
	return sess, sess.Close, nil
}

// ---------------------------------------------------------------------------
// Tool: compare_models
// ---------------------------------------------------------------------------

func handleCompareModels(ctx context.Context, args map[string]any) (any, error) {
	rawModels, ok := args["models"].([]any)
	if !ok || len(rawModels) == 0 {
		return nil, fmt.Errorf("'models' must be a non-empty list of paths")
	}
	featuresPath, err := libvmaf.ValidatePath(strArg(args, "features", ""))
	if err != nil {
		return nil, fmt.Errorf("features: %w", err)
	}
	opts := modeleval.Options{
		FeaturesPath: featuresPath,
		Split:        strArg(args, "split", modeleval.SplitTest),
		InputName:    strArg(args, "input_name", modeleval.DefaultInputName),
	}
	if err := modeleval.ValidateSplit(opts.Split); err != nil {
		return nil, err
	}

	// Resolve every model path up front so a rejected path is reported
	// per-model (under "errors") rather than aborting the whole call,
	// which is what _compare_models does.
	models := make([]string, 0, len(rawModels))
	result := &modeleval.Comparison{Ranked: []*modeleval.Result{}, Errors: []modeleval.ModelError{}}
	for _, m := range rawModels {
		mStr, _ := m.(string)
		mp, err := libvmaf.ValidatePath(mStr)
		if err != nil {
			result.Errors = append(result.Errors, modeleval.ModelError{Model: mStr, Error: err.Error()})
			continue
		}
		models = append(models, mp)
	}

	got := modeleval.Compare(ctx, newDNNPredictor, models, opts)
	got.Errors = append(result.Errors, got.Errors...)
	return got, nil
}

// ---------------------------------------------------------------------------
// Tool: describe_worst_frames
// ---------------------------------------------------------------------------

func handleDescribeWorstFrames(ctx context.Context, args map[string]any) (any, error) {
	ref, err := libvmaf.ValidatePath(strArg(args, "ref", ""))
	if err != nil {
		return nil, fmt.Errorf("ref: %w", err)
	}
	dis, err := libvmaf.ValidatePath(strArg(args, "dis", ""))
	if err != nil {
		return nil, fmt.Errorf("dis: %w", err)
	}
	width := intArg(args, "width", 0)
	height := intArg(args, "height", 0)
	pixfmt := strArg(args, "pixfmt", "420")
	bitdepth := intArg(args, "bitdepth", 8)
	model := strArg(args, "model", "version=vmaf_v0.6.1")
	backend := strArg(args, "backend", "auto")
	n := intArg(args, "n", 5)

	score, err := runVmafScore(ctx, ref, dis, width, height, pixfmt, bitdepth, model, backend, "legacy", scoreExtras{}) // %.6f per ADR-0119
	if err != nil {
		return nil, err
	}
	worst := pickWorstFrames(score, n)

	// Extract PNGs via ffmpeg and return metadata (VLM description is not
	// available in the Go implementation — it returns a placeholder message
	// consistent with "vlm extras not installed" in the Python server).
	tmpDir, err := os.MkdirTemp("", "vmaf-mcp-worst-*")
	if err != nil {
		return nil, fmt.Errorf("failed to create temp dir: %w", err)
	}
	// Note: tmpDir is intentionally not removed here — the Python server
	// also leaves PNGs for the caller to access; they are purged on the
	// next describe_worst_frames call.

	fmtMap := map[[2]any]string{
		{"420", 8}: "yuv420p", {"422", 8}: "yuv422p", {"444", 8}: "yuv444p",
		{"420", 10}: "yuv420p10le", {"422", 10}: "yuv422p10le", {"444", 10}: "yuv444p10le",
	}
	pixFmtFFmpeg, ok := fmtMap[[2]any{pixfmt, bitdepth}]
	if !ok {
		return nil, fmt.Errorf("unsupported pixfmt/bitdepth: %s/%d", pixfmt, bitdepth)
	}

	var outFrames []map[string]any
	for _, fw := range worst {
		frameIdx := fw[0].(int)
		vmafScore := fw[1].(float64)
		pngPath := filepath.Join(tmpDir, fmt.Sprintf("frame_%06d.png", frameIdx))

		extractErr := extractFramePNG(ctx, dis, pngPath, width, height, pixFmtFFmpeg, frameIdx)
		desc := "(VLM unavailable — Go implementation does not include vlm extras; " +
			"use the Python vmaf-mcp server for VLM descriptions)"
		if extractErr != nil {
			desc = fmt.Sprintf("(frame extraction failed: %v)", extractErr)
			pngPath = ""
		}
		outFrames = append(outFrames, map[string]any{
			"frame_index": frameIdx,
			"vmaf":        vmafScore,
			"png":         pngPath,
			"description": desc,
		})
	}
	return map[string]any{
		"model_id": nil,
		"frames":   outFrames,
	}, nil
}

// pickWorstFrames returns the n frames with the lowest VMAF score.
func pickWorstFrames(payload map[string]any, n int) [][2]any {
	frames, _ := payload["frames"].([]any)
	type fScore struct {
		idx   int
		score float64
	}
	var scored []fScore
	for _, f := range frames {
		fm, _ := f.(map[string]any)
		idx := int(floatFromAny(fm["frameNum"]))
		metrics, _ := fm["metrics"].(map[string]any)
		var score float64
		for _, key := range []string{"vmaf", "vmaf_v0.6.1", "vmaf_v0.6.1neg", "vmaf_4k_v0.6.1"} {
			if v, ok := metrics[key]; ok {
				score = floatFromAny(v)
				break
			}
		}
		scored = append(scored, fScore{idx, score})
	}
	sort.Slice(scored, func(i, j int) bool { return scored[i].score < scored[j].score })
	if n > len(scored) {
		n = len(scored)
	}
	result := make([][2]any, n)
	for i := range n {
		result[i] = [2]any{scored[i].idx, scored[i].score}
	}
	return result
}

func floatFromAny(v any) float64 {
	switch n := v.(type) {
	case float64:
		return n
	case json.Number:
		f, _ := n.Float64()
		return f
	}
	return 0
}

func extractFramePNG(ctx context.Context, yuv, outPNG string, width, height int, pixFmt string, frameIdx int) error {
	argv := []string{
		"-loglevel", "error",
		"-f", "rawvideo",
		"-pix_fmt", pixFmt,
		"-s", fmt.Sprintf("%dx%d", width, height),
		"-i", yuv,
		"-vf", fmt.Sprintf("select='eq(n,%d)'", frameIdx),
		"-vsync", "0",
		"-frames:v", "1",
		"-y", outPNG,
	}
	// #nosec G204 -- "ffmpeg" is a fixed binary name; argv values originate
	// from libvmaf.ValidatePath-filtered YUV paths and integer geometry
	// fields validated by extractFramePNG's caller (handleDescribeWorstFrames).
	cmd := exec.CommandContext(ctx, "ffmpeg", argv...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("ffmpeg: %w: %s", err, string(out))
	}
	return nil
}

// ---------------------------------------------------------------------------
// Tool: probe_backend
// ---------------------------------------------------------------------------

// probeYUVData is a 64x64 1-frame 4:2:0 8-bit YUV filled with mid-grey.
// 64x64 x 1.5 = 6144 bytes. 32x32 was insufficient for CUDA ADM, which
// requires at least 36px in each dimension; a 32px frame silently returned a
// null score from the ADM kernel, which the old code misreported as
// runtime_healthy=true (ADR-0608 follow-up). Must stay byte-identical to the
// Python server's _PROBE_YUV_DATA (server.py).
const (
	probeYUVWidth  = 64
	probeYUVHeight = 64
)

var probeYUVData = bytes.Repeat([]byte{128}, probeYUVWidth*probeYUVHeight*3/2)

func handleProbeBackend(ctx context.Context, args map[string]any) (any, error) {
	backend := strArg(args, "backend", "")
	if backend == "" {
		return nil, fmt.Errorf("backend is required")
	}
	vmafBin := libvmaf.FindBinary()
	advertised := probeBackends(vmafBin)
	compiledIn := backend == "cpu" || advertised[backend]

	if !compiledIn {
		return map[string]any{
			"backend":         backend,
			"compiled_in":     false,
			"runtime_healthy": false,
			"latency_ms":      nil,
			"score":           nil,
			"error":           fmt.Sprintf("backend %q is not compiled into the local vmaf binary", backend),
		}, nil
	}

	tmpDir, err := os.MkdirTemp("", "vmaf-mcp-probe-*")
	if err != nil {
		return nil, fmt.Errorf("failed to create temp dir: %w", err)
	}
	defer os.RemoveAll(tmpDir)

	refYUV := filepath.Join(tmpDir, "ref.yuv")
	disYUV := filepath.Join(tmpDir, "dis.yuv")
	outJSON := filepath.Join(tmpDir, "out.json")
	if err := os.WriteFile(refYUV, probeYUVData, 0o600); err != nil {
		return nil, err
	}
	if err := os.WriteFile(disYUV, probeYUVData, 0o600); err != nil {
		return nil, err
	}

	argv := []string{
		"-r", refYUV, "-d", disYUV,
		"--width", strconv.Itoa(probeYUVWidth), "--height", strconv.Itoa(probeYUVHeight),
		"-p", "420", "-b", "8",
		"-m", "version=vmaf_v0.6.1",
		"--precision", "legacy", // %.6f — matches C CLI default (ADR-0119)
		"-q", "-o", outJSON, "--json",
	}
	if siblings, ok := backendDisable[backend]; ok {
		for _, s := range siblings {
			argv = append(argv, "--no_"+s)
		}
	}

	t0 := time.Now()
	// #nosec G204 -- vmafBin resolved via libvmaf.FindBinary (env-overridable
	// to allowlist); argv values are tmpDir paths from os.MkdirTemp + literal
	// flag arguments, no caller-controlled input.
	// Use CommandContext so a hung GPU driver probe can be cancelled by the
	// MCP client (r5-mcp-streaming finding :788, ADR-1018).
	cmd := exec.CommandContext(ctx, vmafBin, argv...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	runErr := cmd.Run()
	latencyMs := float64(time.Since(t0).Microseconds()) / 1000.0

	if runErr != nil {
		return map[string]any{
			"backend":         backend,
			"compiled_in":     compiledIn,
			"runtime_healthy": false,
			"latency_ms":      roundF(latencyMs, 1),
			"score":           nil,
			"error":           fmt.Sprintf("vmaf exited %v: %s", runErr, truncate(stderr.String(), 500)),
		}, nil
	}

	// #nosec G304 -- outJSON is an os.MkdirTemp-derived path joined with a
	// literal filename ("out.json"); not caller-controlled.
	data, err := os.ReadFile(outJSON)
	if err != nil {
		return map[string]any{
			"backend":         backend,
			"compiled_in":     compiledIn,
			"runtime_healthy": false,
			"latency_ms":      roundF(latencyMs, 1),
			"score":           nil,
			"error":           fmt.Sprintf("failed to read output: %v", err),
		}, nil
	}
	var payload map[string]any
	if err := json.Unmarshal(data, &payload); err != nil {
		return map[string]any{
			"backend":         backend,
			"compiled_in":     compiledIn,
			"runtime_healthy": false,
			"latency_ms":      roundF(latencyMs, 1),
			"score":           nil,
			"error":           fmt.Sprintf("failed to parse output: %v", err),
		}, nil
	}
	pooled, _ := payload["pooled_metrics"].(map[string]any)
	vmafPool, _ := pooled["vmaf"].(map[string]any)
	score := vmafPool["mean"]

	// runtime_healthy requires a non-null, finite score: a null score
	// indicates the backend kernel failed silently (e.g. ADM sub-minimum
	// resolution, driver absent) even though the process returned exit code 0.
	// Mirrors the Python server's `score is not None` guard (server.py), and
	// additionally rejects non-finite values (NaN/Inf) defensively.
	healthy := scoreIsHealthy(score)
	var probeErr any
	if !healthy {
		probeErr = "vmaf returned exit 0 but score was null"
	}

	return map[string]any{
		"backend":         backend,
		"compiled_in":     compiledIn,
		"runtime_healthy": healthy,
		"latency_ms":      roundF(latencyMs, 1),
		"score":           score,
		"error":           probeErr,
	}, nil
}

// scoreIsHealthy reports whether a probe score represents a successful run.
// It mirrors the Python server's `score is not None` check (so the JSON output
// stays byte-compatible) and additionally rejects non-finite float values.
func scoreIsHealthy(score any) bool {
	if score == nil {
		return false
	}
	switch n := score.(type) {
	case float64:
		return !math.IsNaN(n) && !math.IsInf(n, 0)
	case json.Number:
		f, err := n.Float64()
		if err != nil {
			return false
		}
		return !math.IsNaN(f) && !math.IsInf(f, 0)
	default:
		// Non-numeric, non-nil value: treat as present (matches Python, which
		// only checks `is not None`).
		return true
	}
}

func roundF(f float64, decimals int) float64 {
	p := 1.0
	for range decimals {
		p *= 10
	}
	return float64(int(f*p+0.5)) / p
}

func truncate(s string, n int) string {
	if len(s) > n {
		return s[:n]
	}
	return s
}

// ---------------------------------------------------------------------------
// Tool: vmaf_version
// ---------------------------------------------------------------------------

func handleVmafVersion(_ context.Context, _ map[string]any) (any, error) {
	vmafBin := libvmaf.FindBinary()
	if _, err := os.Stat(vmafBin); err != nil {
		return map[string]any{
			"binary_path": vmafBin,
			"version":     nil,
			"build_flags": map[string]bool{
				"cpu": false, "cuda": false, "sycl": false,
				"hip": false, "metal": false,
			},
			"error": fmt.Sprintf("vmaf binary not found at %s", vmafBin),
		}, nil
	}

	var versionStr *string
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	// #nosec G204 -- vmafBin resolved via libvmaf.FindBinary; "--version" is literal.
	out, err := exec.CommandContext(ctx, vmafBin, "--version").CombinedOutput()
	if err == nil {
		blob := string(out)
		re := regexp.MustCompile(`(\d+\.\d+[\w.\-]*)`)
		if m := re.FindString(blob); m != "" {
			versionStr = &m
		}
	}

	advertised := probeBackends(vmafBin)
	return map[string]any{
		"binary_path": vmafBin,
		"version":     versionStr,
		"build_flags": map[string]bool{
			"cpu":   true,
			"cuda":  advertised["cuda"],
			"sycl":  advertised["sycl"],
			"hip":   advertised["hip"],
			"metal": advertised["metal"],
		},
		"error": nil,
	}, nil
}

// ---------------------------------------------------------------------------
// Tool: vmaf_score_encoded
// ---------------------------------------------------------------------------

func handleVmafScoreEncoded(ctx context.Context, args map[string]any) (any, error) {
	refPath, err := libvmaf.ValidatePath(strArg(args, "reference_encoded", ""))
	if err != nil {
		return nil, fmt.Errorf("reference_encoded: %w", err)
	}
	disPath, err := libvmaf.ValidatePath(strArg(args, "distorted_encoded", ""))
	if err != nil {
		return nil, fmt.Errorf("distorted_encoded: %w", err)
	}
	model := strArg(args, "model", "version=vmaf_v0.6.1")
	backend := strArg(args, "backend", "auto")
	if !validBackends[backend] {
		return nil, fmt.Errorf("invalid backend %q: must be one of auto|cpu|cuda|sycl|hip|metal", backend)
	}
	precision := strArg(args, "precision", "legacy") // "legacy"=%.6f matches C CLI default (ADR-0119)
	extras, err := parseScoreExtras(args)
	if err != nil {
		return nil, err
	}
	if extras.noReference && extras.tinyModel == "" {
		return nil, fmt.Errorf("no_reference requires tiny_model; no classic NR scorer exists")
	}

	width, height, pixfmt, bitdepth, err := ffprobeGeometry(refPath)
	if err != nil {
		return nil, fmt.Errorf("ffprobe failed on reference: %w", err)
	}

	ffmpegPixfmt, err := toFFmpegPixfmt(pixfmt, bitdepth)
	if err != nil {
		return nil, err
	}

	tmpDir, err := os.MkdirTemp("", "vmaf-mcp-encoded-*")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(tmpDir)

	refYUV := filepath.Join(tmpDir, "ref.yuv")
	disYUV := filepath.Join(tmpDir, "dis.yuv")

	// Decode both in parallel.
	errCh := make(chan error, 2)
	go func() { errCh <- decodeToYUV(ctx, refPath, refYUV, ffmpegPixfmt) }()
	go func() { errCh <- decodeToYUV(ctx, disPath, disYUV, ffmpegPixfmt) }()
	for range 2 {
		if e := <-errCh; e != nil {
			return nil, e
		}
	}

	result, err := runVmafScore(ctx, refYUV, disYUV, width, height, pixfmt, bitdepth, model, backend, precision, extras)
	if err != nil {
		return nil, err
	}
	result["reference_encoded"] = refPath
	result["distorted_encoded"] = disPath
	return result, nil
}

func ffprobeGeometry(path string) (width, height int, pixfmt string, bitdepth int, err error) {
	args := []string{
		"-v", "quiet",
		"-select_streams", "v:0",
		"-show_entries", "stream=width,height,pix_fmt",
		"-of", "json",
		path,
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	// #nosec G204 -- "ffprobe" is a fixed binary; `path` is the libvmaf.ValidatePath
	// output already filtered to AllowedRoots by handleVmafScoreEncoded.
	out, e := exec.CommandContext(ctx, "ffprobe", args...).Output()
	if e != nil {
		return 0, 0, "", 0, fmt.Errorf("ffprobe: %w", e)
	}
	var info struct {
		Streams []struct {
			Width  int    `json:"width"`
			Height int    `json:"height"`
			PixFmt string `json:"pix_fmt"`
		} `json:"streams"`
	}
	if e := json.Unmarshal(out, &info); e != nil {
		return 0, 0, "", 0, e
	}
	if len(info.Streams) == 0 {
		return 0, 0, "", 0, fmt.Errorf("no video stream found in %s", path)
	}
	s := info.Streams[0]
	pixfmtMap := map[string][2]any{
		"yuv420p": {"420", 8}, "yuvj420p": {"420", 8},
		"yuv422p": {"422", 8}, "yuvj422p": {"422", 8},
		"yuv444p": {"444", 8}, "yuvj444p": {"444", 8},
		"yuv420p10le": {"420", 10}, "yuv422p10le": {"422", 10}, "yuv444p10le": {"444", 10},
		"yuv420p12le": {"420", 12}, "yuv422p12le": {"422", 12}, "yuv444p12le": {"444", 12},
		"yuv420p10be": {"420", 10}, "yuv422p10be": {"422", 10}, "yuv444p10be": {"444", 10},
	}
	mapped, ok := pixfmtMap[s.PixFmt]
	if !ok {
		return 0, 0, "", 0, fmt.Errorf("unsupported pix_fmt %q", s.PixFmt)
	}
	return s.Width, s.Height, mapped[0].(string), mapped[1].(int), nil
}

func toFFmpegPixfmt(pixfmt string, bitdepth int) (string, error) {
	m := map[[2]any]string{
		{"420", 8}: "yuv420p", {"422", 8}: "yuv422p", {"444", 8}: "yuv444p",
		{"420", 10}: "yuv420p10le", {"422", 10}: "yuv422p10le", {"444", 10}: "yuv444p10le",
		{"420", 12}: "yuv420p12le", {"422", 12}: "yuv422p12le", {"444", 12}: "yuv444p12le",
	}
	v, ok := m[[2]any{pixfmt, bitdepth}]
	if !ok {
		return "", fmt.Errorf("unsupported pixfmt/bitdepth: %s/%d", pixfmt, bitdepth)
	}
	return v, nil
}

func decodeToYUV(ctx context.Context, src, dst, pixFmt string) error {
	argv := []string{
		"-loglevel", "error",
		"-i", src,
		"-f", "rawvideo",
		"-pix_fmt", pixFmt,
		"-y", dst,
	}
	// #nosec G204 -- "ffmpeg" is a fixed binary; `src` originates from
	// libvmaf.ValidatePath via handleVmafScoreEncoded, `dst` is an
	// os.MkdirTemp output path, pixFmt comes from a fixed lookup table
	// (toFFmpegPixfmt).
	cmd := exec.CommandContext(ctx, "ffmpeg", argv...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("ffmpeg decode of %s: %w: %s", src, err, string(out))
	}
	return nil
}

// ---------------------------------------------------------------------------
// Tool: list_extractors
// ---------------------------------------------------------------------------

var fexStructRe = regexp.MustCompile(
	`VmafFeatureExtractor\s+vmaf_fex_(\w+)\s*=\s*\{[^{]*?\.name\s*=\s*"([^"]+)"`,
)

var backendKeywords = []struct{ suffix, label string }{
	{"_cuda", "cuda"}, {"_sycl", "sycl"},
	{"_hip", "hip"}, {"_metal", "metal"},
}

func inferBackendFromSym(sym string) string {
	for _, kw := range backendKeywords {
		if strings.HasSuffix(sym, kw.suffix) {
			return kw.label
		}
	}
	return "cpu"
}

func handleListExtractors(_ context.Context, _ map[string]any) (any, error) {
	root := libvmaf.RepoRoot()
	featureDir := filepath.Join(root, "libvmaf", "src", "feature")
	// After ADR-0700 the dir moved to core/src/feature.
	if _, err := os.Stat(featureDir); err != nil {
		featureDir = filepath.Join(root, "core", "src", "feature")
	}

	seen := map[[2]string]bool{}
	var out []map[string]any

	err := filepath.WalkDir(featureDir, func(path string, d fs.DirEntry, walkErr error) error {
		if walkErr != nil || d.IsDir() {
			return nil
		}
		if !strings.HasSuffix(path, ".c") {
			return nil
		}
		// Defensive: ensure the WalkDir-supplied path is still inside the
		// trusted feature dir (defeats symlinks that point outside the tree
		// — gosec G122 / CWE-367 TOCTOU). The feature dir is the fork's own
		// in-tree source, so this guard is precautionary.
		absPath, absErr := filepath.Abs(path)
		if absErr != nil {
			return nil //nolint:nilerr // skip unreadable entries
		}
		absRoot, rootErr := filepath.Abs(featureDir)
		if rootErr != nil || !strings.HasPrefix(absPath, absRoot+string(os.PathSeparator)) {
			return nil
		}
		data, err := os.ReadFile(absPath) // #nosec G304,G122 -- absPath validated inside trusted root above
		if err != nil {
			return nil //nolint:nilerr
		}
		for _, m := range fexStructRe.FindAllSubmatch(data, -1) {
			sym := string(m[1])
			name := string(m[2])
			key := [2]string{sym, name}
			if seen[key] {
				continue
			}
			seen[key] = true
			rel, _ := filepath.Rel(root, path)
			out = append(out, map[string]any{
				"name":    name,
				"backend": inferBackendFromSym(sym),
				"source":  rel,
			})
		}
		return nil
	})
	if err != nil {
		return nil, fmt.Errorf("failed to walk feature dir: %w", err)
	}
	return map[string]any{"extractors": out}, nil
}

// ---------------------------------------------------------------------------
// Tool: describe_model
// ---------------------------------------------------------------------------

var modelExtensions = map[string]bool{".json": true, ".pkl": true, ".onnx": true}

func stripModelExt(filename string) string {
	ext := strings.ToLower(filepath.Ext(filename))
	if modelExtensions[ext] {
		return filename[:len(filename)-len(ext)]
	}
	return filename
}

func handleDescribeModel(_ context.Context, args map[string]any) (any, error) {
	nameOrPath := strArg(args, "name", "")
	if nameOrPath == "" {
		return nil, fmt.Errorf("name is required")
	}
	// When VMAFX_MCP_DIRECT=1, augment the base payload with a libvmaf
	// schema-validation step (ADR-0931 Phase 1).
	if directPathEnabled() {
		return describeModelDirect(nameOrPath)
	}
	return describeModel(nameOrPath)
}

func describeModel(nameOrPath string) (map[string]any, error) {
	root := libvmaf.RepoRoot()
	modelsDir := filepath.Join(root, "model")

	// Step 1: try as a direct path. Validate via libvmaf.ValidatePath so the
	// caller cannot reach files outside the allowlisted roots (e.g. via "../"
	// traversal joined onto root).
	candidate := nameOrPath
	if !filepath.IsAbs(candidate) {
		candidate = filepath.Join(root, candidate)
	}
	candidate, _ = filepath.Abs(candidate)
	if validated, vErr := libvmaf.ValidatePath(candidate); vErr == nil {
		ext := strings.ToLower(filepath.Ext(validated))
		if modelExtensions[ext] {
			return describeModelFile(validated, root)
		}
	}

	// Step 2: search model/ by filename stem.
	var matches []string
	err := filepath.WalkDir(modelsDir, func(path string, d fs.DirEntry, e error) error {
		if e != nil || d.IsDir() {
			return nil
		}
		ext := strings.ToLower(filepath.Ext(path))
		if !modelExtensions[ext] {
			return nil
		}
		stem := stripModelExt(filepath.Base(path))
		if stem == nameOrPath || filepath.Base(path) == nameOrPath {
			matches = append(matches, path)
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	if len(matches) == 0 {
		return nil, fmt.Errorf("model %q not found; run list_models to see available models", nameOrPath)
	}
	if len(matches) > 1 {
		var paths []string
		for _, m := range matches {
			rel, _ := filepath.Rel(root, m)
			paths = append(paths, rel)
		}
		return nil, fmt.Errorf("model %q is ambiguous; matched: %v", nameOrPath, paths)
	}
	return describeModelFile(matches[0], root)
}

func describeModelFile(path, root string) (map[string]any, error) {
	fi, err := os.Stat(path)
	if err != nil {
		return nil, err
	}
	ext := strings.ToLower(filepath.Ext(path))
	stem := stripModelExt(filepath.Base(path))
	rel, _ := filepath.Rel(root, path)
	result := map[string]any{
		"name":          stem,
		"path":          rel,
		"format":        strings.TrimPrefix(ext, "."),
		"size_bytes":    fi.Size(),
		"model_type":    nil,
		"feature_names": nil,
	}
	if ext == ".json" {
		// #nosec G304 -- describeModelFile is only reachable from describeModel
		// which validates the path via libvmaf.ValidatePath OR walks the
		// in-repo model/ dir (filepath.WalkDir).
		data, err := os.ReadFile(path)
		if err == nil {
			var payload map[string]any
			if json.Unmarshal(data, &payload) == nil {
				if md, ok := payload["model_dict"].(map[string]any); ok {
					result["model_type"] = md["model_type"]
					result["feature_names"] = md["feature_names"]
				}
			}
		}
	}
	return result, nil
}

// ---------------------------------------------------------------------------
// Tool: run_compare
// ---------------------------------------------------------------------------

func handleRunCompare(ctx context.Context, args map[string]any) (any, error) {
	vmaftune := findVmafTune()
	if _, err := os.Stat(vmaftune); err != nil {
		return nil, fmt.Errorf(
			"vmaf-tune binary not found at %s. Install: pip install -e tools/vmaf-tune", vmaftune,
		)
	}

	argv := []string{"compare", "--src", strArg(args, "src", ""), "--format", "json"}
	if hasArg(args, "target_vmafs") {
		argv = append(argv, "--target-vmafs", strArg(args, "target_vmafs", ""))
	} else if hasArg(args, "target_vmaf") {
		argv = append(argv, "--target-vmaf", strconv.FormatFloat(floatArg(args, "target_vmaf", 0), 'f', -1, 64))
	}
	if hasArg(args, "encoders") {
		argv = append(argv, "--encoders", strArg(args, "encoders", ""))
	}
	if hasArg(args, "width") {
		argv = append(argv, "--width", strconv.Itoa(intArg(args, "width", 0)))
	}
	if hasArg(args, "height") {
		argv = append(argv, "--height", strconv.Itoa(intArg(args, "height", 0)))
	}
	argv = append(argv, "--pix-fmt", strArg(args, "pix_fmt", "yuv420p"))
	if hasArg(args, "framerate") {
		argv = append(argv, "--framerate", strconv.FormatFloat(floatArg(args, "framerate", 0), 'f', -1, 64))
	}
	if boolArg(args, "no_parallel", false) {
		argv = append(argv, "--no-parallel")
	}

	// #nosec G204 -- vmaftune is resolved via findVmafTune (fixed candidate
	// list); argv values are flag-prefixed strings derived from JSON-Schema
	// validated tool arguments.
	out, err := runVmafTune(ctx, vmaftune, "compare", argv)
	if err != nil {
		return nil, err
	}
	var result any
	if err := json.Unmarshal(out, &result); err != nil {
		return map[string]any{"exit_code": 0, "stdout": string(out), "stderr": ""}, nil
	}
	return result, nil
}

// ---------------------------------------------------------------------------
// Tool: run_ladder
// ---------------------------------------------------------------------------

func handleRunLadder(ctx context.Context, args map[string]any) (any, error) {
	vmaftune := findVmafTune()
	if _, err := os.Stat(vmaftune); err != nil {
		return nil, fmt.Errorf(
			"vmaf-tune binary not found at %s. Install: pip install -e tools/vmaf-tune", vmaftune,
		)
	}

	format := strArg(args, "format", "json")
	argv := []string{
		"ladder",
		"--src", strArg(args, "src", ""),
		"--encoder", strArg(args, "encoder", "libx264"),
		"--resolutions", strArg(args, "resolutions", ""),
		"--target-vmafs", strArg(args, "target_vmafs", ""),
		"--quality-tiers", strconv.Itoa(intArg(args, "quality_tiers", 5)),
		"--format", format,
		"--spacing", strArg(args, "spacing", "log_bitrate"),
	}
	if hasArg(args, "framerate") {
		argv = append(argv, "--framerate", strconv.FormatFloat(floatArg(args, "framerate", 0), 'f', -1, 64))
	}

	// #nosec G204 -- vmaftune resolved via findVmafTune; argv values are
	// schema-validated tool arguments prefixed by literal flags.
	out, err := runVmafTune(ctx, vmaftune, "ladder", argv)
	if err != nil {
		return nil, err
	}
	if format == "json" {
		var manifest any
		if err := json.Unmarshal(out, &manifest); err == nil {
			return map[string]any{"manifest": manifest, "format": format}, nil
		}
	}
	return map[string]any{"manifest": string(out), "format": format}, nil
}

// ---------------------------------------------------------------------------
// Tool: run_tune_per_shot
// ---------------------------------------------------------------------------

func handleRunTunePerShot(ctx context.Context, args map[string]any) (any, error) {
	vmaftune := findVmafTune()
	if _, err := os.Stat(vmaftune); err != nil {
		return nil, fmt.Errorf(
			"vmaf-tune binary not found at %s. Install: pip install -e tools/vmaf-tune", vmaftune,
		)
	}

	format := strArg(args, "format", "json")
	argv := []string{
		"tune-per-shot",
		"--src", strArg(args, "src", ""),
		"--target-vmaf", strconv.FormatFloat(floatArg(args, "target_vmaf", 92.0), 'f', -1, 64),
		"--encoder", strArg(args, "encoder", "libx264"),
		"--pix-fmt", strArg(args, "pix_fmt", "yuv420p"),
		// NOTE: tune-per-shot has no --format flag (unlike `compare`); passing
		// it made argparse error on every call. The `format` arg still selects
		// how we parse the output below, matching the Python server.
	}
	if hasArg(args, "output") {
		argv = append(argv, "--output", strArg(args, "output", ""))
	}
	if hasArg(args, "framerate") {
		argv = append(argv, "--framerate", strconv.FormatFloat(floatArg(args, "framerate", 0), 'f', -1, 64))
	}
	if hasArg(args, "scene_threshold") {
		argv = append(argv, "--scene-threshold", strconv.FormatFloat(floatArg(args, "scene_threshold", 0), 'f', -1, 64))
	}

	// #nosec G204 -- vmaftune resolved via findVmafTune; argv values are
	// schema-validated tool arguments prefixed by literal flags.
	out, err := runVmafTune(ctx, vmaftune, "tune-per-shot", argv)
	if err != nil {
		return nil, err
	}
	if format == "json" {
		var result any
		if err := json.Unmarshal(out, &result); err == nil {
			return result, nil
		}
	}
	return map[string]any{"exit_code": 0, "stdout": string(out), "stderr": ""}, nil
}

// runVmafTune runs the vmaf-tune binary with the given subcommand argv and
// returns its stdout. On a non-zero exit it captures stderr and folds it into
// the wrapped error, mirroring the Python server's error text
// ("vmaf-tune <sub> exited <rc>: <stderr>") so callers don't lose the failure
// diagnostics that bare exec.Output() drops. ctx propagates client-disconnect
// cancellation (ADR-1085 invariant #9).
//
// #nosec G204 -- vmaftune is resolved by the caller via findVmafTune (fixed
// candidate list / VMAF_TUNE_BIN override); argv values are flag-prefixed
// strings derived from JSON-Schema validated tool arguments.
func runVmafTune(ctx context.Context, vmaftune, sub string, argv []string) ([]byte, error) {
	cmd := exec.CommandContext(ctx, vmaftune, argv...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		detail := strings.TrimSpace(stderr.String())
		if detail == "" {
			detail = err.Error()
		}
		return nil, fmt.Errorf("vmaf-tune %s exited %v: %s", sub, err, detail)
	}
	return out, nil
}

// ---------------------------------------------------------------------------
// vmaf-tune binary resolution
// ---------------------------------------------------------------------------

func findVmafTune() string {
	if env := os.Getenv("VMAF_TUNE_BIN"); env != "" {
		return env
	}
	if p, err := exec.LookPath("vmaf-tune"); err == nil {
		return p
	}
	return filepath.Join(libvmaf.RepoRoot(), "tools", "vmaf-tune", "vmaf-tune")
}
