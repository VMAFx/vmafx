// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

// tools.go registers the 15 VMAFX MCP tools. Tool names, argument schemas,
// and response shapes are byte-for-byte compatible with the Python vmaf-mcp
// server so that IDE MCP clients (Claude Desktop, Cursor) work unchanged.
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

	"github.com/modelcontextprotocol/go-sdk/mcp"
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
func scoringExtraProperties() schemaObj {
	return schemaObj{
		// --- Feature selection + CTC presets ---
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
		// --- Tiny-AI / DNN scoring surface (ADR-1117) ---
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
		// --- Score-param completeness ---
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

// registerTools wires every MCP tool into srv. Schema definitions mirror the
// Python server's _list_tools() output exactly.
func registerTools(srv *mcp.Server) {
	addRawTool(srv, &mcp.Tool{
		Name: "vmaf_score",
		Description: "Compute a VMAF score for a (reference, distorted) YUV pair. " +
			"Optional tiny-AI/DNN, feature-selection, CTC-preset, and frame-range " +
			"parameters map onto the corresponding vmaf CLI flags (ADR-1117).",
		InputSchema: mustSchema(schemaObj{
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
				// "legacy" = %.6f, matching C CLI default per ADR-0119; use "max" for lossless.
				"precision": schemaObj{"type": "string", "default": "legacy"},
			}, scoringExtraProperties()),
		}),
	}, handleVmafScore)

	addRawTool(srv, &mcp.Tool{
		Name:        "list_models",
		Description: "Enumerate VMAF models (JSON / pickle / ONNX) shipped with the repo.",
		InputSchema: mustSchema(schemaObj{"type": "object", "properties": schemaObj{}}),
	}, handleListModels)

	addRawTool(srv, &mcp.Tool{
		Name: "list_backends",
		Description: "Report which runtime backends (cpu / cuda / sycl / hip / metal) " +
			"the local vmaf binary was built with.",
		InputSchema: mustSchema(schemaObj{"type": "object", "properties": schemaObj{}}),
	}, handleListBackends)

	addRawTool(srv, &mcp.Tool{
		Name: "run_benchmark",
		Description: "Run the full multi-fixture benchmark harness (bench_all.sh) " +
			"across all available backends (CPU, CUDA, SYCL) on " +
			"three canonical YUV fixture sets: the 576x324 Netflix golden " +
			"pair, a 1080p 5-frame pair, and the 4K BBB 200-frame pair. " +
			"Returns stdout (per-backend scores + backend comparison table) " +
			"and stderr. Takes no arguments — fixtures are built-in. " +
			"ADR-0513.",
		InputSchema: mustSchema(schemaObj{"type": "object", "properties": schemaObj{}}),
	}, handleRunBenchmark)

	addRawTool(srv, &mcp.Tool{
		Name: "eval_model_on_split",
		Description: "Run an ONNX tiny-AI regressor on a parquet feature cache, " +
			"filter to a deterministic train/val/test split (keyed by the " +
			"'key' column), and report PLCC / SROCC / RMSE.",
		InputSchema: mustSchema(schemaObj{
			"type":     "object",
			"required": []string{"model", "features"},
			"properties": schemaObj{
				"model":      schemaObj{"type": "string", "description": "ONNX model path."},
				"features":   schemaObj{"type": "string", "description": "Parquet feature cache path."},
				"split":      schemaObj{"type": "string", "enum": []string{"train", "val", "test", "all"}, "default": "test"},
				"input_name": schemaObj{"type": "string", "default": "features"},
			},
		}),
	}, handleEvalModelOnSplit)

	addRawTool(srv, &mcp.Tool{
		Name: "compare_models",
		Description: "Rank several ONNX models on the same parquet feature split by " +
			"descending PLCC. Models that fail to load or score are listed " +
			"under 'errors' instead of aborting the whole call.",
		InputSchema: mustSchema(schemaObj{
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
		}),
	}, handleCompareModels)

	addRawTool(srv, &mcp.Tool{
		Name: "describe_worst_frames",
		Description: "Score a (ref, dis) pair, pick the N worst-VMAF frames, extract " +
			"each as PNG via ffmpeg, and run a vision-language model " +
			"(SmolVLM -> Moondream2 fallback) to describe the visible " +
			"artefacts. Falls back to metadata-only output when the [vlm] " +
			"extras are not installed. ADR-0172 / T6-6.",
		InputSchema: mustSchema(schemaObj{
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
		}),
	}, handleDescribeWorstFrames)

	addRawTool(srv, &mcp.Tool{
		Name: "probe_backend",
		Description: "Run a 1-frame VMAF health check to distinguish 'compiled in' from " +
			"'driver present + functional'. Returns compiled_in (bool), " +
			"runtime_healthy (bool), latency_ms, score (the VMAF mean on a " +
			"64x64 mid-grey pair; >=36px per dimension is required by CUDA ADM), " +
			"and any error string. Use this when " +
			"list_backends returns true but actual GPU dispatch may fail " +
			"(driver not loaded, ICD missing, KFD ioctl failure, etc.). " +
			"ADR-0608.",
		InputSchema: mustSchema(schemaObj{
			"type":     "object",
			"required": []string{"backend"},
			"properties": schemaObj{
				"backend": schemaObj{
					"type":        "string",
					"enum":        []string{"cpu", "cuda", "sycl", "hip", "metal"},
					"description": "Backend to health-check.",
				},
			},
		}),
	}, handleProbeBackend)

	addRawTool(srv, &mcp.Tool{
		Name: "vmaf_version",
		Description: "Return the local vmaf binary's identity and build flags. " +
			"Reports binary_path, version string (from --version), and " +
			"build_flags dict (cpu/cuda/sycl/hip/metal). Use this " +
			"to confirm which fork build is running before scoring. " +
			"ADR-0608.",
		InputSchema: mustSchema(schemaObj{"type": "object", "properties": schemaObj{}}),
	}, handleVmafVersion)

	addRawTool(srv, &mcp.Tool{
		Name: "vmaf_score_encoded",
		Description: "Score a (reference, distorted) pair of encoded video files " +
			"(MP4, MKV, Y4M, WebM, etc.) by decoding them to raw YUV via " +
			"ffmpeg and then running vmaf_score. Geometry (width, height, " +
			"pixel format, bit depth) is probed automatically from the " +
			"reference stream — no manual size entry required. Returns the " +
			"same response shape as vmaf_score plus reference_encoded and " +
			"distorted_encoded fields. Requires ffmpeg + ffprobe on PATH. " +
			"ADR-0608.",
		InputSchema: mustSchema(schemaObj{
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
				// "legacy" = %.6f, matching C CLI default per ADR-0119; use "max" for lossless.
				"precision": schemaObj{"type": "string", "default": "legacy"},
			}, scoringExtraProperties()),
		}),
	}, handleVmafScoreEncoded)

	addRawTool(srv, &mcp.Tool{
		Name: "list_extractors",
		Description: "Enumerate all VmafFeatureExtractor implementations found in the " +
			"local libvmaf C source tree. Returns each extractor's advertised " +
			"name, inferred backend (cpu / cuda / sycl / hip / metal), " +
			"and the source file it was defined in. Requires no binary — " +
			"parses the C source directly. ADR-0608.",
		InputSchema: mustSchema(schemaObj{"type": "object", "properties": schemaObj{}}),
	}, handleListExtractors)

	addRawTool(srv, &mcp.Tool{
		Name: "describe_model",
		Description: "Return metadata for a VMAF model by name or path. Accepts the " +
			"model's filename stem (e.g. 'vmaf_v0.6.1'), its full filename " +
			"(e.g. 'vmaf_v0.6.1.json'), or a path relative to the repo root. " +
			"Fixes the Path.stem bug: 'vmaf_v0.6.1' is matched correctly " +
			"against 'vmaf_v0.6.1.json' — not mis-trimmed to 'vmaf_v0.6'. " +
			"Returns: name, path, format, size_bytes, model_type (JSON only), " +
			"feature_names (JSON only). ADR-0608.",
		InputSchema: mustSchema(schemaObj{
			"type":     "object",
			"required": []string{"name"},
			"properties": schemaObj{
				"name": schemaObj{
					"type": "string",
					"description": "Model name stem (e.g. 'vmaf_v0.6.1'), full filename " +
						"(e.g. 'vmaf_v0.6.1.json'), or repo-relative path.",
				},
			},
		}),
	}, handleDescribeModel)

	addRawTool(srv, &mcp.Tool{
		Name: "run_compare",
		Description: "Wrap 'vmaf-tune compare': compare codec adapters at one or more " +
			"target VMAF scores and return a ranked report. Requires vmaf-tune " +
			"to be installed (pip install -e tools/vmaf-tune). Emits MCP " +
			"progress notifications when params._meta.progressToken is set. " +
			"Default encoders: libx264,libx265,libsvtav1,libvpx-vp9. " +
			"ADR-0608.",
		InputSchema: mustSchema(schemaObj{
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
		}),
	}, handleRunCompare)

	addRawTool(srv, &mcp.Tool{
		Name: "run_ladder",
		Description: "Wrap 'vmaf-tune ladder': build a per-title bitrate ladder via " +
			"convex-hull sweep over resolution x target-VMAF, pick K knees, " +
			"and emit an HLS / DASH / JSON manifest. Requires vmaf-tune. " +
			"Emits MCP progress notifications. ADR-0608.",
		InputSchema: mustSchema(schemaObj{
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
		}),
	}, handleRunLadder)

	addRawTool(srv, &mcp.Tool{
		Name: "run_tune_per_shot",
		Description: "Wrap 'vmaf-tune tune-per-shot': detect scene cuts, run a " +
			"per-shot CRF bisect targeting a VMAF score, and return the " +
			"encoding plan. Requires vmaf-tune. Emits MCP progress " +
			"notifications. ADR-0608.",
		InputSchema: mustSchema(schemaObj{
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
		}),
	}, handleRunTunePerShot)
}

// addRawTool adds a tool with a raw JSON inputSchema.
func addRawTool(srv *mcp.Server, tool *mcp.Tool, handler func(context.Context, map[string]any) (any, error)) {
	srv.AddTool(tool, func(ctx context.Context, req *mcp.CallToolRequest) (*mcp.CallToolResult, error) {
		args, err := parseArgs(req)
		if err != nil {
			return errorResult(fmt.Sprintf("invalid arguments: %v", err)), nil
		}
		result, err := handler(ctx, args)
		if err != nil {
			return errorResult(err.Error()), nil
		}
		text, err := json.MarshalIndent(result, "", "  ")
		if err != nil {
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
	for k, v := range base {
		out[k] = v
	}
	for k, v := range extra {
		out[k] = v
	}
	return out
}

// mustSchema converts a Go map to a json.RawMessage for use as an InputSchema.
// Panics on marshal failure (schema is always a compile-time constant).
func mustSchema(v any) json.RawMessage {
	b, err := json.Marshal(v)
	if err != nil {
		panic(fmt.Sprintf("mustSchema: %v", err))
	}
	return b
}
