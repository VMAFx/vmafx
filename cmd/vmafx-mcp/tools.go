// Copyright 2026 Lusoris. All rights reserved.
// SPDX-License-Identifier: EUPL-1.2

// tools.go registers the 24 VMAFX MCP tools across 8 functional categories:
//  1. Metric computation & scoring (2): vmaf_score, vmaf_score_encoded
//  2. Model discovery & inspection (3): list_models, describe_model, compare_models
//  3. Platform & hardware inspection (4): list_backends, list_extractors, probe_backend, vmaf_version
//  4. Frame-level diagnosis & ML (2): describe_worst_frames, eval_model_on_split
//  5. Benchmarking (1): run_benchmark
//  6. Tuning & optimization CLI wrappers (3): run_compare, run_ladder, run_tune_per_shot
//  7. Sidecar-binary bridge (4): vmaf_per_shot, vmaf_roi, vmaf_bench, vmaf_vpl
//  8. Phase-4b gRPC control-plane bridge (5, Go-only per ADR-1173): submit_job,
//     get_job, cancel_job, list_jobs, vmaf_score_remote
// Total: 2 + 3 + 4 + 2 + 1 + 3 + 4 + 5 = 24 tools. Categories 1-6 (19 tools with
// the sidecars) have byte-compatible Python twins; category 8 is Go-only.
// Stale historical comments referenced 16 tools prior to model inspection tool
// consolidation into compare_models.
// Tool names, argument schemas, and response shapes are byte-for-byte compatible
// with the Python vmaf-mcp server so that IDE MCP clients (Claude Desktop, Cursor)
// work unchanged. The one deliberate exception is the gRPC bridge (category 8),
// which has no Python twin — see ADR-1173.
//
// Each tool is implemented by a corresponding function in impl.go that calls
// out to the vmaf CLI binary. The Go server does NOT link against libvmaf.so
// at runtime — it delegates scoring to the binary, identical to the Python
// implementation.

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"maps"

	"github.com/modelcontextprotocol/go-sdk/mcp"

	"github.com/VMAFx/vmafx/pkg/observability"
)

// schemaObj is a shorthand for a raw JSON object used as inputSchema.
type schemaObj = map[string]any

// scoringExtraProperties returns the optional pass-through scoring parameters
// shared by vmaf_score and vmaf_score_encoded. They map onto the `vmaf` CLI
// flags verified against core/tools/cli_parse.c (ADR-1117). Every property is
// optional and only forwarded to the CLI when the caller supplies it, so
// existing callers are unaffected (backward-compatible).
//
// This function MUST stay byte-identical to the Python server's
// `_scoring_extra_properties()` (mcp-server/vmaf-mcp/src/vmaf_mcp/server.py)
// — same keys, enums, defaults, and descriptions — per cmd/vmafx-mcp/AGENTS.md.
//
// The set is assembled from the same groups the Python server lays out in
// `_scoring_extra_properties()`. Splitting it across those groups changes no key,
// enum, default or description -- only where each group is written -- so the
// byte-identical contract above is unaffected.
func scoringExtraProperties() schemaObj {
	out := schemaObj{}
	for _, group := range []schemaObj{
		scoringFeatureProperties(),
		scoringTinyModelProperties(),
		scoringTinyTuningProperties(),
		scoringFrameRangeProperties(),
		scoringDeviceProperties(),
		scoringOutputProperties(),
	} {
		maps.Copy(out, group)
	}
	return out
}

// scoringFeatureProperties returns the feature-selection and CTC-preset pass-through properties.
func scoringFeatureProperties() schemaObj {
	return schemaObj{
		"feature": schemaObj{
			"type":  "array",
			"items": schemaObj{"type": "string"},
			"description": "Additional feature extractors, each passed as a repeated " +
				"--feature flag. Use the libvmaf 'name[=key=val:...]' syntax, e.g. " +
				"'psnr' or 'cambi=full_ref=true'. Mutually exclusive with aom_ctc/nflx_ctc.",
		},
		"aom_ctc": schemaObj{
			"type": "string",
			"enum": []string{"v1.0", "v2.0", "v3.0", "v4.0", "v5.0", "v6.0", "v7.0"},
			"description": "AOM Common Test Conditions preset (--aom_ctc). Configures a fixed " +
				"model + feature set; mutually exclusive with manual feature/model config.",
		},
		"nflx_ctc": schemaObj{
			"type": "string",
			"enum": []string{"v1.0"},
			"description": "Netflix Common Test Conditions preset (--nflx_ctc). Mutually " +
				"exclusive with manual feature/model config.",
		},
	}
}

// scoringTinyModelProperties returns the tiny-AI model and execution-provider properties.
func scoringTinyModelProperties() schemaObj {
	return schemaObj{
		"tiny_model": schemaObj{
			"type":        "string",
			"description": "Path to a tiny ONNX model loaded alongside classic models (--tiny-model).",
		},
		"tiny_device": schemaObj{
			"type": "string",
			"enum": []string{
				"auto", "cpu", "cuda", "openvino", "openvino-npu", "openvino-cpu",
				"openvino-gpu", "coreml", "coreml-ane", "coreml-gpu", "coreml-cpu", "rocm",
			},
			"description": "ONNX Runtime execution provider for the tiny model (--tiny-device / " +
				"--dnn-ep). Default: auto.",
		},
		"dnn_ep": schemaObj{
			"type": "string",
			"enum": []string{
				"auto", "cpu", "cuda", "openvino", "openvino-npu", "openvino-cpu",
				"openvino-gpu", "coreml", "coreml-ane", "coreml-gpu", "coreml-cpu", "rocm",
			},
			"description": "Alias for tiny_device: ONNX Runtime execution provider for the " +
				"tiny model (--dnn-ep / --tiny-device). Default: auto.",
		},
	}
}

// scoringTinyTuningProperties returns the tiny-AI tuning properties, including the no-reference switch.
func scoringTinyTuningProperties() schemaObj {
	return schemaObj{
		"tiny_threads": schemaObj{
			"type":        "integer",
			"minimum":     0,
			"description": "CPU EP intra-op thread count for the tiny model (--tiny-threads; 0 = ORT default).",
		},
		"tiny_fp16": schemaObj{
			"type":        "boolean",
			"description": "Request fp16 IO where the execution provider supports it (--tiny-fp16).",
		},
		"tiny_model_verify": schemaObj{
			"type":        "boolean",
			"description": "Require Sigstore-bundle verification of the tiny model before use (--tiny-model-verify).",
		},
		"tiny_codec": schemaObj{
			"type":        "string",
			"description": "Encoder name for codec-aware tiny models (--tiny-codec), e.g. libx264.",
		},
		"tiny_preset": schemaObj{
			"type":        "string",
			"description": "Encoder preset string for codec-aware tiny models (--tiny-preset).",
		},
		"tiny_crf": schemaObj{
			"type":        "integer",
			"minimum":     0,
			"maximum":     63,
			"description": "CRF / QP integer for codec-aware tiny models (--tiny-crf; clamped to 0..63).",
		},
		"tiny_resize": schemaObj{
			"type": "string",
			"enum": []string{"bilinear", "nearest", "bicubic", "disabled"},
			"description": "Auto-resize filter for NCHW tiny models on dimension mismatch " +
				"(--tiny-resize). Default: disabled (mismatch hard-errors).",
		},
		"no_reference": schemaObj{
			"type": "boolean",
			"description": "No-reference (NR) mode (--no-reference). Requires tiny_model (an NR " +
				"ONNX model); the reference path becomes a formality — pass any valid YUV " +
				"of matching geometry since only the distorted picture is scored.",
		},
	}
}

// scoringFrameRangeProperties returns the thread-count and frame-range properties.
func scoringFrameRangeProperties() schemaObj {
	return schemaObj{
		"threads": schemaObj{
			"type":        "integer",
			"minimum":     1,
			"description": "Worker thread count (--threads). Capped to hardware cores by the CLI.",
		},
		"frame_cnt": schemaObj{
			"type":        "integer",
			"minimum":     1,
			"description": "Maximum number of frames to process (--frame_cnt).",
		},
		"frame_skip_ref": schemaObj{
			"type":        "integer",
			"minimum":     0,
			"description": "Skip the first N frames of the reference (--frame_skip_ref).",
		},
		"frame_skip_dist": schemaObj{
			"type":        "integer",
			"minimum":     0,
			"description": "Skip the first N frames of the distorted input (--frame_skip_dist).",
		},
		"no_prediction": schemaObj{
			"type":        "boolean",
			"description": "Extract features only, skip VMAF prediction (--no_prediction).",
		},
	}
}

// scoringDeviceProperties returns the CPU and GPU device-selector properties.
func scoringDeviceProperties() schemaObj {
	return schemaObj{
		"cpumask": schemaObj{
			"type":        "integer",
			"minimum":     0,
			"description": "Bitmask restricting permitted CPU SIMD instruction sets (--cpumask).",
		},
		"gpumask": schemaObj{
			"type":        "integer",
			"minimum":     0,
			"description": "Bitmask restricting permitted GPU operations (--gpumask).",
		},
		"sycl_device": schemaObj{
			"type":        "integer",
			"minimum":     0,
			"description": "Select SYCL GPU device by index (--sycl_device).",
		},
		"hip_device": schemaObj{
			"type":        "integer",
			"minimum":     0,
			"description": "Select HIP GPU device by index (--hip_device).",
		},
		"metal_device": schemaObj{
			"type":        "integer",
			"minimum":     0,
			"description": "Select Metal GPU device by index (--metal_device).",
		},
	}
}

// scoringOutputProperties returns the output-format and model-flag properties.
func scoringOutputProperties() schemaObj {
	return schemaObj{
		"output_fmt": schemaObj{
			"type":        "string",
			"enum":        []string{"json", "xml", "csv", "sub"},
			"default":     "json",
			"description": "Score output format (--json, --xml, --csv, --sub). Default: json.",
		},
		// --- Model flags & score-param leftovers ---
		"disable_clip": schemaObj{
			"type":        "boolean",
			"description": "Disable score clipping to [0, 100] on the model (--model ...:disable_clip).",
		},
		"enable_transform": schemaObj{
			"type":        "boolean",
			"description": "Enable score transform on the model (--model ...:enable_transform).",
		},
		"csv": schemaObj{
			"type":        "boolean",
			"description": "Write output file as CSV (--csv). Equivalent to output_fmt='csv'.",
		},
		"sub": schemaObj{
			"type":        "boolean",
			"description": "Write output file as subtitle-style per-frame scores (--sub). Equivalent to output_fmt='sub'.",
		},
	}
}

// registerTools wires every MCP tool into srv. Schema definitions mirror the
// Python server's _list_tools() output exactly.
//
// The registrations are grouped into the helpers below, called in the order the
// registrations were originally written, so the tool set and its registration order are
// both unchanged by the grouping. A new tool belongs in one of these helpers: a register
// function that nothing calls registers nothing, and the parity test only checks that
// every Python tool is present, so the omission would not be caught there.
//
// A schema that fails to marshal aborts the whole registration: the returned error is
// fatal to buildServer, so the process never serves a tool surface in which one tool's
// declared schema is missing or weaker than the one written here.
func registerTools(srv *mcp.Server) error {
	reg := &toolRegistrar{srv: srv}
	registerScoringTools(reg)
	registerModelEvalTools(reg)
	registerFrameInspectionTools(reg)
	registerEncodedScoringTools(reg)
	registerCatalogTools(reg)
	registerCompareTool(reg)
	registerLadderTool(reg)
	registerTunePerShotTool(reg)
	registerPerShotTool(reg)
	registerRoiTool(reg)
	registerBenchTool(reg)
	registerVplTool(reg)
	registerJobSubmissionTools(reg)
	registerJobControlTools(reg)
	registerRemoteScoringTool(reg)
	return reg.err
}

// registerScoringTools wires the core scoring tool and the model/backend listings.
func registerScoringTools(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "vmaf_score",
		Description: "Compute a VMAF score for a (reference, distorted) YUV pair. " +
			"Optional tiny-AI/DNN, feature-selection, CTC-preset, and frame-range " +
			"parameters map onto the corresponding vmaf CLI flags (ADR-1117).",
	}, schemaObj{
		"type": "object",
		"required": []string{
			"ref", "dis", "width", "height", "pixfmt", "bitdepth",
		},
		"properties": mergeSchema(schemaObj{
			"ref":      schemaObj{"type": "string", "description": "Reference YUV path."},
			"dis":      schemaObj{"type": "string", "description": "Distorted YUV path."},
			"width":    schemaObj{"type": "integer", "minimum": 1},
			"height":   schemaObj{"type": "integer", "minimum": 1},
			"pixfmt":   schemaObj{"type": "string", "enum": []string{"420", "422", "444"}},
			"bitdepth": schemaObj{"type": "integer", "enum": []int{8, 10, 12, 16}},
			"model":    schemaObj{"type": "string", "default": "version=vmaf_v0.6.1"},
			"backend": schemaObj{
				"type":    "string",
				"enum":    []string{"auto", "cpu", "cuda", "sycl", "hip", "metal"},
				"default": "auto",
			},
			"subsample": schemaObj{
				"type":        "integer",
				"minimum":     1,
				"default":     1,
				"description": "Score every Nth frame (1 = every frame).",
			},
			"precision": schemaObj{
				"type":        "string",
				"default":     "legacy",
				"description": "Score precision format. Default 'legacy' (%.6f, Netflix-compatible per ADR-0119); use 'max' for lossless float output (%.17g).",
			},
		}, scoringExtraProperties()),
	}, handleVmafScore)

	reg.add(&mcp.Tool{
		Name:        "list_models",
		Description: "Enumerate VMAF models (JSON / pickle / ONNX) shipped with the repo.",
	}, schemaObj{"type": "object", "properties": schemaObj{}}, handleListModels)

	reg.add(&mcp.Tool{
		Name: "list_backends",
		Description: "Report which runtime backends (cpu / cuda / sycl / hip / metal) " +
			"the local vmaf binary was built with.",
	}, schemaObj{"type": "object", "properties": schemaObj{}}, handleListBackends)
}

// registerModelEvalTools wires the benchmark harness and the ONNX model evaluation tools.
func registerModelEvalTools(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "run_benchmark",
		Description: "Run the full multi-fixture benchmark harness (bench_all.sh) " +
			"across all available backends (CPU, CUDA, SYCL) on " +
			"three canonical YUV fixture sets: the 576x324 Netflix golden " +
			"pair, a 1080p 5-frame pair, and the 4K BBB 200-frame pair. " +
			"Returns stdout (per-backend scores + backend comparison table) " +
			"and stderr. Takes no arguments — fixtures are built-in. " +
			"ADR-0513.",
	}, schemaObj{"type": "object", "properties": schemaObj{}}, handleRunBenchmark)

	reg.add(&mcp.Tool{
		Name: "eval_model_on_split",
		Description: "Run an ONNX tiny-AI regressor on a parquet feature cache, " +
			"filter to a deterministic train/val/test split (keyed by the " +
			"'key' column), and report PLCC / SROCC / RMSE.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"model", "features"},
		"properties": schemaObj{
			"model":      schemaObj{"type": "string", "description": "ONNX model path."},
			"features":   schemaObj{"type": "string", "description": "Parquet feature cache path."},
			"split":      schemaObj{"type": "string", "enum": []string{"train", "val", "test", "all"}, "default": "test"},
			"input_name": schemaObj{"type": "string", "default": "features"},
		},
	}, handleEvalModelOnSplit)

	reg.add(&mcp.Tool{
		Name: "compare_models",
		Description: "Rank several ONNX models on the same parquet feature split by " +
			"descending PLCC. Models that fail to load or score are listed " +
			"under 'errors' instead of aborting the whole call.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"models", "features"},
		"properties": schemaObj{
			"models": schemaObj{
				"type":     "array",
				"items":    schemaObj{"type": "string"},
				"minItems": 1,
			},
			"features":   schemaObj{"type": "string"},
			"split":      schemaObj{"type": "string", "enum": []string{"train", "val", "test", "all"}, "default": "test"},
			"input_name": schemaObj{"type": "string", "default": "features"},
		},
	}, handleCompareModels)
}

// registerFrameInspectionTools wires the worst-frame description and backend probe tools.
func registerFrameInspectionTools(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "describe_worst_frames",
		Description: "Score a (ref, dis) pair, pick the N worst-VMAF frames, extract " +
			"each as PNG via ffmpeg, and run a vision-language model " +
			"(SmolVLM -> Moondream2 fallback) to describe the visible " +
			"artefacts. Falls back to metadata-only output when the [vlm] " +
			"extras are not installed. ADR-0172 / T6-6.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"ref", "dis", "width", "height", "pixfmt", "bitdepth"},
		"properties": schemaObj{
			"ref":      schemaObj{"type": "string"},
			"dis":      schemaObj{"type": "string"},
			"width":    schemaObj{"type": "integer", "minimum": 1},
			"height":   schemaObj{"type": "integer", "minimum": 1},
			"pixfmt":   schemaObj{"type": "string", "enum": []string{"420", "422", "444"}},
			"bitdepth": schemaObj{"type": "integer", "enum": []int{8, 10, 12, 16}},
			"model":    schemaObj{"type": "string", "default": "version=vmaf_v0.6.1"},
			"backend": schemaObj{
				"type":    "string",
				"enum":    []string{"auto", "cpu", "cuda", "sycl", "hip", "metal"},
				"default": "auto",
			},
			"n": schemaObj{
				"type":        "integer",
				"minimum":     1,
				"maximum":     32,
				"default":     5,
				"description": "How many worst-VMAF frames to describe.",
			},
		},
	}, handleDescribeWorstFrames)

	reg.add(&mcp.Tool{
		Name: "probe_backend",
		Description: "Run a 1-frame VMAF health check to distinguish 'compiled in' from " +
			"'driver present + functional'. Returns compiled_in (bool), " +
			"runtime_healthy (bool), latency_ms, score (the VMAF mean on a " +
			"64x64 mid-grey pair; >=36px per dimension is required by CUDA ADM), " +
			"and any error string. Use this when " +
			"list_backends returns true but actual GPU dispatch may fail " +
			"(driver not loaded, ICD missing, KFD ioctl failure, etc.). " +
			"ADR-0608.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"backend"},
		"properties": schemaObj{
			"backend": schemaObj{
				"type":        "string",
				"enum":        []string{"cpu", "cuda", "sycl", "hip", "metal"},
				"description": "Backend to health-check.",
			},
		},
	}, handleProbeBackend)
}

// registerEncodedScoringTools wires the version report and the encode-then-score tool.
func registerEncodedScoringTools(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "vmaf_version",
		Description: "Return the local vmaf binary's identity and build flags. " +
			"Reports binary_path, version string (from --version), and " +
			"build_flags dict (cpu/cuda/sycl/hip/metal). Use this " +
			"to confirm which fork build is running before scoring. " +
			"ADR-0608.",
	}, schemaObj{"type": "object", "properties": schemaObj{}}, handleVmafVersion)

	reg.add(&mcp.Tool{
		Name: "vmaf_score_encoded",
		Description: "Score a (reference, distorted) pair of encoded video files " +
			"(MP4, MKV, Y4M, WebM, etc.) by decoding them to raw YUV via " +
			"ffmpeg and then running vmaf_score. Geometry (width, height, " +
			"pixel format, bit depth) is probed automatically from the " +
			"reference stream — no manual size entry required. Returns the " +
			"same response shape as vmaf_score plus reference_encoded and " +
			"distorted_encoded fields. Requires ffmpeg + ffprobe on PATH. " +
			"ADR-0608.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"reference_encoded", "distorted_encoded"},
		"properties": mergeSchema(schemaObj{
			"reference_encoded": schemaObj{
				"type": "string",
				"description": "Path to the reference encoded video (MP4/MKV/Y4M/...). " +
					"Must be under an allowlisted root (VMAF_MCP_ALLOW).",
			},
			"distorted_encoded": schemaObj{
				"type":        "string",
				"description": "Path to the distorted encoded video.",
			},
			"model": schemaObj{"type": "string", "default": "version=vmaf_v0.6.1"},
			"backend": schemaObj{
				"type":    "string",
				"enum":    []string{"auto", "cpu", "cuda", "sycl", "hip", "metal"},
				"default": "auto",
			},
			"subsample": schemaObj{
				"type":        "integer",
				"minimum":     1,
				"default":     1,
				"description": "Score every Nth frame (1 = every frame).",
			},
			"precision": schemaObj{
				"type":        "string",
				"default":     "legacy",
				"description": "Score precision format. Default 'legacy' (%.6f, Netflix-compatible per ADR-0119); use 'max' for lossless float output (%.17g).",
			},
		}, scoringExtraProperties()),
	}, handleVmafScoreEncoded)
}

// registerCatalogTools wires the feature-extractor listing and the model description tool.
func registerCatalogTools(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "list_extractors",
		Description: "Enumerate all VmafFeatureExtractor implementations found in the " +
			"local libvmaf C source tree. Returns each extractor's advertised " +
			"name, inferred backend (cpu / cuda / sycl / hip / metal), " +
			"and the source file it was defined in. Requires no binary — " +
			"parses the C source directly. ADR-0608.",
	}, schemaObj{"type": "object", "properties": schemaObj{}}, handleListExtractors)

	reg.add(&mcp.Tool{
		Name: "describe_model",
		Description: "Return metadata for a VMAF model by name or path. Accepts the " +
			"model's filename stem (e.g. 'vmaf_v0.6.1'), its full filename " +
			"(e.g. 'vmaf_v0.6.1.json'), or a path relative to the repo root. " +
			"Fixes the Path.stem bug: 'vmaf_v0.6.1' is matched correctly " +
			"against 'vmaf_v0.6.1.json' — not mis-trimmed to 'vmaf_v0.6'. " +
			"Returns: name, path, format, size_bytes, model_type (JSON only), " +
			"feature_names (JSON only). ADR-0608.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"name"},
		"properties": schemaObj{
			"name": schemaObj{
				"type": "string",
				"description": "Model name stem (e.g. 'vmaf_v0.6.1'), full filename " +
					"(e.g. 'vmaf_v0.6.1.json'), or repo-relative path.",
			},
		},
	}, handleDescribeModel)
}

// registerCompareTool wires the encoder-parameter sweep tool.
func registerCompareTool(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "run_compare",
		Description: "Wrap 'vmaf-tune compare': compare codec adapters at one or more " +
			"target VMAF scores and return a ranked report. Requires vmaf-tune " +
			"to be installed (pip install -e tools/vmaf-tune). Emits MCP " +
			"progress notifications when params._meta.progressToken is set. " +
			"Default encoders: libx264,libx265,libsvtav1,libvpx-vp9. " +
			"ADR-0608.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"src"},
		"properties": schemaObj{
			"src": schemaObj{
				"type":        "string",
				"description": "Source video path (any FFmpeg-readable format or raw YUV).",
			},
			"target_vmaf": schemaObj{
				"type":        "number",
				"description": "Single VMAF target (legacy, single-target schema).",
			},
			"target_vmafs": schemaObj{
				"type":        "string",
				"description": "Comma-separated VMAF targets, e.g. '94,96,97,98'.",
			},
			"encoders": schemaObj{
				"type":        "string",
				"description": "Comma-separated encoder list, e.g. 'libx264,libx265'.",
			},
			"width":     schemaObj{"type": "integer", "description": "Source width (raw YUV only)."},
			"height":    schemaObj{"type": "integer", "description": "Source height (raw YUV only)."},
			"pix_fmt":   schemaObj{"type": "string", "default": "yuv420p"},
			"framerate": schemaObj{"type": "number", "description": "Source framerate."},
			"no_parallel": schemaObj{
				"type":        "boolean",
				"default":     false,
				"description": "Dispatch encoders sequentially (default: parallel).",
			},
		},
	}, handleRunCompare)
}

// registerLadderTool wires the bitrate-ladder tool.
func registerLadderTool(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "run_ladder",
		Description: "Wrap 'vmaf-tune ladder': build a per-title bitrate ladder via " +
			"convex-hull sweep over resolution x target-VMAF, pick K knees, " +
			"and emit an HLS / DASH / JSON manifest. Requires vmaf-tune. " +
			"Emits MCP progress notifications. ADR-0608.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"src", "resolutions", "target_vmafs"},
		"properties": schemaObj{
			"src":          schemaObj{"type": "string", "description": "Source video path."},
			"resolutions":  schemaObj{"type": "string", "description": "Comma-separated WxH list, e.g. '1920x1080,1280x720,854x480'."},
			"target_vmafs": schemaObj{"type": "string", "description": "Comma-separated VMAF targets, e.g. '95,90,85'."},
			"encoder": schemaObj{
				"type":        "string",
				"default":     "libx264",
				"description": "Codec adapter (default libx264).",
			},
			"quality_tiers": schemaObj{
				"type":        "integer",
				"default":     5,
				"description": "Number of ladder rungs to select.",
			},
			"format": schemaObj{
				"type":        "string",
				"enum":        []string{"hls", "dash", "json"},
				"default":     "json",
				"description": "Manifest format.",
			},
			"spacing": schemaObj{
				"type":    "string",
				"enum":    []string{"log_bitrate", "vmaf", "uniform"},
				"default": "log_bitrate",
			},
			"framerate": schemaObj{"type": "number", "description": "Source framerate."},
		},
	}, handleRunLadder)
}

// registerTunePerShotTool wires the vmafx-tune per-shot driver tool.
func registerTunePerShotTool(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "run_tune_per_shot",
		Description: "Wrap 'vmaf-tune tune-per-shot': detect scene cuts, run a " +
			"per-shot CRF bisect targeting a VMAF score, and return the " +
			"encoding plan. Requires vmaf-tune. Emits MCP progress " +
			"notifications. ADR-0608.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"src"},
		"properties": schemaObj{
			"src":             schemaObj{"type": "string", "description": "Source video path."},
			"target_vmaf":     schemaObj{"type": "number", "default": 92.0, "description": "Target VMAF score."},
			"encoder":         schemaObj{"type": "string", "default": "libx264"},
			"pix_fmt":         schemaObj{"type": "string", "default": "yuv420p"},
			"framerate":       schemaObj{"type": "number"},
			"scene_threshold": schemaObj{"type": "number", "description": "Scene-cut detection threshold (0..1)."},
			"output":          schemaObj{"type": "string", "description": "Output video path (optional; plan-only if omitted)."},
			"format": schemaObj{
				"type":    "string",
				"enum":    []string{"json", "shell", "csv"},
				"default": "json",
			},
		},
	}, handleRunTunePerShot)
}

// registerPerShotTool wires the per-shot scoring tool.
func registerPerShotTool(reg *toolRegistrar) {
	// ── Sidecar-binary bridge (#1240 item b) ────────────────────────────
	// One tool per sidecar CLI built next to `vmaf` in core/tools/. Schemas
	// mirror each binary's own --help exactly; bounds match the C parsers.

	reg.add(&mcp.Tool{
		Name: "vmaf_per_shot",
		Description: "Wrap the 'vmaf-perShot' sidecar binary: scan a raw YUV reference, " +
			"detect shot boundaries from luma complexity + motion energy, and " +
			"return a per-shot CRF plan targeting a VMAF score. Returns the " +
			"parsed JSON plan by default (format='csv' returns the raw CSV " +
			"text instead). ADR-0222.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"reference", "width", "height"},
		"properties": schemaObj{
			"reference": schemaObj{
				"type":        "string",
				"description": "Reference raw planar YUV path (must be under an allowlisted root).",
			},
			"width":  schemaObj{"type": "integer", "minimum": 16, "maximum": 65535},
			"height": schemaObj{"type": "integer", "minimum": 16, "maximum": 65535},
			"pixel_format": schemaObj{
				"type": "string", "enum": []string{"420", "422", "444"}, "default": "420",
				"description": "Planar YUV subsampling (--pixel_format).",
			},
			"bitdepth": schemaObj{
				"type": "integer", "enum": []int{8, 10, 12, 16}, "default": 8,
				"description": "Planar YUV bit depth (--bitdepth).",
			},
			"target_vmaf": schemaObj{
				"type": "number", "minimum": 0, "maximum": 100, "default": 90,
				"description": "Target VMAF score the CRF predictor aims at (--target-vmaf).",
			},
			"crf_min": schemaObj{
				"type": "integer", "minimum": 0, "maximum": 63, "default": 18,
				"description": "Lower CRF clamp (--crf-min). Must not exceed crf_max.",
			},
			"crf_max": schemaObj{
				"type": "integer", "minimum": 0, "maximum": 63, "default": 35,
				"description": "Upper CRF clamp (--crf-max).",
			},
			"diff_threshold": schemaObj{
				"type": "number", "minimum": 0, "maximum": 255,
				"description": "Shot-detector frame-diff cutoff (--diff-threshold; C default 12).",
			},
			"format": schemaObj{
				"type": "string", "enum": []string{"json", "csv"}, "default": "json",
				"description": "Plan encoding (--format). The MCP tool defaults to json " +
					"(the C CLI defaults to csv) so the plan comes back structured.",
			},
		},
	}, handleVmafPerShot)
}

// registerRoiTool wires the region-of-interest scoring tool.
func registerRoiTool(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "vmaf_roi",
		Description: "Wrap the 'vmaf_roi' sidecar binary: compute a per-CTU saliency grid " +
			"for one frame of a raw YUV file and emit an encoder ROI sidecar. " +
			"encoder='x265' returns the qpfile text in 'qpfile'; encoder='svt-av1' " +
			"returns the raw int8 ROI map base64-encoded in 'roi_map_base64'. " +
			"Without saliency_model a centre-weighted radial placeholder is used " +
			"(smoke-test quality only).",
	}, schemaObj{
		"type":     "object",
		"required": []string{"reference", "width", "height", "frame"},
		"properties": schemaObj{
			"reference": schemaObj{
				"type":        "string",
				"description": "Reference raw planar YUV path (must be under an allowlisted root).",
			},
			"width":  schemaObj{"type": "integer", "minimum": 1, "maximum": 16384},
			"height": schemaObj{"type": "integer", "minimum": 1, "maximum": 16384},
			"frame": schemaObj{
				"type": "integer", "minimum": 0, "maximum": 1000000,
				"description": "0-based frame index to score (--frame).",
			},
			"pixel_format": schemaObj{
				"type": "string", "enum": []string{"420", "422", "444"}, "default": "420",
			},
			"bitdepth": schemaObj{
				"type": "integer", "enum": []int{8, 10, 12, 16}, "default": 8,
			},
			"ctu_size": schemaObj{
				"type": "integer", "minimum": 8, "maximum": 128, "default": 64,
				"description": "CTU grid cell size (--ctu-size; x265 max-ctu).",
			},
			"encoder": schemaObj{
				"type": "string", "enum": []string{"x265", "svt-av1"}, "default": "x265",
				"description": "Sidecar dialect (--encoder).",
			},
			"strength": schemaObj{
				"type": "number", "minimum": 0, "maximum": 64, "default": 6.0,
				"description": "QP-offset gain applied to the saliency grid (--strength).",
			},
			"saliency_model": schemaObj{
				"type": "string",
				"description": "Optional ONNX [1,1,H,W] luma->[0,1] saliency model " +
					"(--saliency-model). Must be under an allowlisted root.",
			},
		},
	}, handleVmafRoi)
}

// registerBenchTool wires the micro-benchmark tool.
func registerBenchTool(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "vmaf_bench",
		Description: "Wrap the 'vmaf_bench' sidecar binary: per-feature micro-benchmark " +
			"over the built-in synthetic fixtures, or (validate=true) a GPU-vs-CPU " +
			"correctness comparison. Distinct from run_benchmark, which runs the " +
			"end-to-end bench_all.sh harness over real YUV fixtures. In validate " +
			"mode a non-zero exit is reported as validation_failed=true rather " +
			"than as a tool error.",
	}, schemaObj{
		"type": "object",
		"properties": schemaObj{
			"frames": schemaObj{
				"type": "integer", "minimum": 2, "maximum": 48,
				"description": "Frames per benchmark (--frames; C default 10, max 48).",
			},
			"resolution": schemaObj{
				"type":        "string",
				"enum":        []string{"576x324", "640x480", "1280x720", "1920x1080", "3840x2160"},
				"description": "Single resolution to test (--resolution). Omit to test all.",
			},
			"bpc": schemaObj{
				"type": "integer", "enum": []int{8, 10, 12, 16},
				"description": "Bits per component (--bpc; C default 8).",
			},
			"data_dir": schemaObj{
				"type": "string",
				"description": "Test-data directory (--data-dir). Must be a directory under " +
					"an allowlisted root.",
			},
			"validate": schemaObj{
				"type": "boolean", "default": false,
				"description": "GPU-vs-CPU correctness comparison (--validate).",
			},
			"gpu_only": schemaObj{
				"type": "boolean", "default": false,
				"description": "Skip the CPU targets (--gpu-only).",
			},
			"device_list": schemaObj{
				"type": "boolean", "default": false,
				"description": "List the available GPU devices and exit (--list-devices).",
			},
		},
	}, handleVmafBench)
}

// registerVplTool wires the oneVPL hardware decode-and-score tool.
func registerVplTool(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "vmaf_vpl",
		Description: "Wrap the 'vmaf_vpl' sidecar binary: decode an encoded (reference, " +
			"distorted) pair with Intel oneVPL, import the VA surfaces zero-copy " +
			"into SYCL via DMA-BUF, and score them. Only built when the oneVPL + " +
			"libva + SYCL toolchain is present; otherwise the tool reports the " +
			"missing binary. Returns the parsed vmaf_score and frames_processed " +
			"alongside the raw stdout.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"ref", "dis"},
		"properties": schemaObj{
			"ref": schemaObj{"type": "string", "description": "Reference encoded video path."},
			"dis": schemaObj{"type": "string", "description": "Distorted encoded video path."},
			"model": schemaObj{
				// The sidecar, not this server, chooses this value.
				"type": "string", "default": "vmaf_v0.6.1", // vmaf-model-pin: vmaf_vpl.c's own --model default
				"description": "Bare VMAF model name (--model). Paths are rejected.",
			},
			"frames": schemaObj{
				"type": "integer", "minimum": 0, "default": 0,
				"description": "Max frames to process (--frames; 0 = all).",
			},
			"device": schemaObj{
				"type": "integer", "minimum": 0, "default": 0,
				"description": "SYCL device index (--device).",
			},
			"render_node": schemaObj{
				"type": "string", "default": "/dev/dri/renderD128",
				"description": "VA-API render node (--render-node). Restricted to " +
					"/dev/dri/renderD<N> or /dev/dri/card<N>.",
			},
			"fallback": schemaObj{
				"type": "boolean", "default": false,
				"description": "Fall back to a host upload when the zero-copy import fails (--fallback).",
			},
		},
	}, handleVmafVpl)
}

// registerJobSubmissionTools wires the controller job submission and lookup tools.
func registerJobSubmissionTools(reg *toolRegistrar) {
	// ── Phase-4b gRPC control-plane bridge (#1240 item c, ADR-1173) ─────
	// Go-only tools: the Python server has no gRPC stack by design. Targets
	// come from the environment (VMAFX_CONTROLLER_ADDR / VMAFX_SERVER_ADDR),
	// never from tool arguments.

	reg.add(&mcp.Tool{
		Name: "submit_job",
		Description: "Enqueue a scoring job on the vmafx-controller (Phase-4b control " +
			"plane) and return its job ID. Paths are resolved by the worker node " +
			"against its shared mount, not by this server, so they must be " +
			"absolute worker-side paths. Target host comes from " +
			"VMAFX_CONTROLLER_ADDR (default localhost:9090). ADR-0711 / ADR-1173.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"reference", "distorted"},
		"properties": schemaObj{
			"reference": schemaObj{
				"type":        "string",
				"description": "Absolute worker-side path to the reference video.",
			},
			"distorted": schemaObj{
				"type":        "string",
				"description": "Absolute worker-side path to the distorted video.",
			},
			"model": schemaObj{
				"type":        "string",
				"description": "VMAF model name. Omit to let the controller apply its own default.",
			},
			"backend": schemaObj{
				"type":    "string",
				"enum":    []string{"auto", "cpu", "cuda", "sycl", "hip", "metal"},
				"default": "auto",
				"description": "Backend capability the scheduler must match on a node. " +
					"'auto' lets the scheduler pick any node.",
			},
		},
	}, handleSubmitJob)

	reg.add(&mcp.Tool{
		Name: "get_job",
		Description: "Fetch the current state of a controller job by ID: status " +
			"(PENDING/RUNNING/COMPLETED/FAILED/CANCELLED), assigned node, error, " +
			"timestamps, final_score, and any partial per-frame results. " +
			"ADR-0711 / ADR-1173.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"job_id"},
		"properties": schemaObj{
			"job_id": schemaObj{"type": "string", "description": "Job UUID returned by submit_job."},
		},
	}, handleGetJob)
}

// registerJobControlTools wires the controller job cancellation and listing tools.
func registerJobControlTools(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "cancel_job",
		Description: "Request cancellation of a PENDING or RUNNING controller job. " +
			"Returns ok=false with a message when the controller declines. " +
			"ADR-0711 / ADR-1173.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"job_id"},
		"properties": schemaObj{
			"job_id": schemaObj{"type": "string", "description": "Job UUID to cancel."},
		},
	}, handleCancelJob)

	reg.add(&mcp.Tool{
		Name: "list_jobs",
		Description: "List the controller's current job snapshot, optionally filtered by " +
			"status. Drains the StreamJobs server-streaming RPC, which sends the " +
			"current snapshot and closes (ADR-0962). Returns at most 'limit' jobs " +
			"and sets truncated=true when more were available. ADR-1173.",
	}, schemaObj{
		"type": "object",
		"properties": schemaObj{
			"status_filter": schemaObj{
				"type": "array",
				"items": schemaObj{
					"type": "string",
					"enum": []string{"PENDING", "RUNNING", "COMPLETED", "FAILED", "CANCELLED"},
				},
				"description": "Restrict the snapshot to these states. Omit for all jobs.",
			},
			"limit": schemaObj{
				"type": "integer", "minimum": 1, "maximum": 500, "default": 100,
				"description": "Maximum jobs to return.",
			},
		},
	}, handleListJobs)
}

// registerRemoteScoringTool wires the server-side scoring tool.
func registerRemoteScoringTool(reg *toolRegistrar) {
	reg.add(&mcp.Tool{
		Name: "vmaf_score_remote",
		Description: "Score a (reference, distorted) pair on a remote vmafx-server over " +
			"the unary VmafxScoring.Score gRPC RPC. Nothing is read locally — the " +
			"paths are resolved by the server, so they must be absolute paths on " +
			"the server's mount. Target host comes from VMAFX_SERVER_ADDR " +
			"(default localhost:9090). Use vmaf_score for local files. " +
			"ADR-0703 / ADR-1173.",
	}, schemaObj{
		"type":     "object",
		"required": []string{"reference", "distorted"},
		"properties": schemaObj{
			"reference": schemaObj{
				"type":        "string",
				"description": "Absolute server-side path to the reference video.",
			},
			"distorted": schemaObj{
				"type":        "string",
				"description": "Absolute server-side path to the distorted video.",
			},
			"model": schemaObj{
				"type":        "string",
				"description": "VMAF model name. Omit to let vmafx-server apply its own default.",
			},
		},
	}, handleVmafScoreRemote)
}

// addRawTool adds a tool with a raw JSON inputSchema.
//
// Every registered tool call runs inside one SpanMCPTool span (ADR-0782)
// tagged with the tool name — this wrapper is the single per-request span
// site for the MCP binary on both transports. The span records the tool's
// failure (argument parse, handler error, marshal error) on the span status
// while the MCP response keeps its IsError contract (a non-nil error is never
// returned to the SDK; see errorResult). The tracer is the global one that
// bootstrap.Base's otel.Module installs, so this is inert when OTel is off.
func addRawTool(srv *mcp.Server, tool *mcp.Tool, handler func(context.Context, map[string]any) (any, error)) {
	srv.AddTool(tool, func(ctx context.Context, req *mcp.CallToolRequest) (*mcp.CallToolResult, error) {
		ctx, span := observability.StartSpan(ctx, observability.SpanMCPTool,
			observability.AttrMCPTool.String(tool.Name))
		var spanErr error
		defer observability.EndSpan(span, &spanErr)

		args, err := parseArgs(req)
		if err != nil {
			spanErr = err
			return errorResult(fmt.Sprintf("invalid arguments: %v", err)), nil
		}
		result, err := handler(ctx, args)
		if err != nil {
			spanErr = err
			return errorResult(err.Error()), nil
		}
		text, err := json.MarshalIndent(result, "", "  ")
		if err != nil {
			spanErr = err
			return errorResult(fmt.Sprintf("failed to marshal result: %v", err)), nil
		}
		return &mcp.CallToolResult{
			Content: []mcp.Content{
				&mcp.TextContent{Text: string(text)},
			},
		}, nil
	})
}

// parseArgs extracts the arguments map from a CallToolRequest.
func parseArgs(req *mcp.CallToolRequest) (map[string]any, error) {
	if req.Params.Arguments == nil {
		return map[string]any{}, nil
	}
	raw, err := json.Marshal(req.Params.Arguments)
	if err != nil {
		return nil, err
	}
	var m map[string]any
	if err := json.Unmarshal(raw, &m); err != nil {
		return nil, err
	}
	return m, nil
}

// errorResult wraps an error message in a CallToolResult with IsError=true.
func errorResult(msg string) *mcp.CallToolResult {
	return &mcp.CallToolResult{
		IsError: true,
		Content: []mcp.Content{
			&mcp.TextContent{Text: msg},
		},
	}
}

// mergeSchema returns a new schemaObj containing all entries from base plus
// every entry from extra. Keys in extra override base on collision (none are
// expected). Used to splice the shared scoring-extra properties into the
// vmaf_score / vmaf_score_encoded property maps without mutating either input.
func mergeSchema(base, extra schemaObj) schemaObj {
	out := make(schemaObj, len(base)+len(extra))
	maps.Copy(out, base)
	maps.Copy(out, extra)
	return out
}

// toolSchema converts a Go map to a json.RawMessage for use as an InputSchema.
//
// Every schema passed here is a literal built from strings, numbers, booleans, slices and
// nested maps, so json.Marshal has no input it can fail on. Should that stop being true,
// the failure is returned, never defaulted: a tool whose declared schema did not marshal
// has no validation at all, so substituting a permissive schema would leave the tool
// reachable with every argument check silently switched off. The caller (toolRegistrar)
// refuses to register the tool and buildServer refuses to start, which is loud and
// recoverable; a tool that accepts anything is neither.
func toolSchema(v any) (json.RawMessage, error) {
	b, err := json.Marshal(v)
	if err != nil {
		return nil, fmt.Errorf("marshalling tool input schema: %w", err)
	}
	return b, nil
}

// toolRegistrar owns the marshalling of every tool input schema and retains the first
// failure. Registration goes through add, which sets InputSchema from the marshalled
// schema itself, so "a tool registered with a schema that never marshalled" is not a
// state a caller can construct: there is no path from a marshal failure to a registered
// tool. Once err is set, later registrations are skipped -- registerTools returns err,
// buildServer discards the half-built server and fails, so the skipped tools are never
// observable -- and the first failure is the one reported, since a schema literal that
// stops marshalling is a source defect to be fixed at its first occurrence.
type toolRegistrar struct {
	srv *mcp.Server
	err error
}

// add marshals schema into tool.InputSchema and registers tool with handler.
//
// A marshal failure registers nothing, records the error against the tool name, and
// leaves every later add a no-op.
func (r *toolRegistrar) add(
	tool *mcp.Tool,
	schema schemaObj,
	handler func(context.Context, map[string]any) (any, error),
) {
	if r.err != nil {
		return
	}
	raw, err := toolSchema(schema)
	if err != nil {
		r.err = fmt.Errorf("tool %q: %w", tool.Name, err)
		return
	}
	tool.InputSchema = raw
	addRawTool(r.srv, tool, handler)
}
