<!-- markdownlint-disable MD013 MD060 -->
# Documentation

This is an overview of the available documentation in the VMAF repository.

## Getting started

- [Installation](getting-started/install/ubuntu.md) – per-OS install scripts (Ubuntu, Fedora, Arch, Alpine, macOS, Windows)
- [Building on Windows](getting-started/building-on-windows.md) – build VMAF from source on Windows (upstream guide)

## Usage

- [CLI reference](usage/cli.md) – the `vmaf` command-line tool, every flag, defaults, examples
- [`vmaf_bench`](usage/bench.md) – micro-benchmark & GPU-vs-CPU validation harness
- [`--precision`](usage/precision.md) – score output precision (default
  `%.6f` Netflix-compat; opt into `%.17g` round-trip lossless via
  `--precision=max`)
- [Python library](usage/python.md) – explains the Python wrapper for VMAF
- [FFmpeg](usage/ffmpeg.md) – how to use VMAF in conjunction with FFmpeg
- [Docker](usage/docker.md) – how to run VMAF with Docker
- [MATLAB](usage/matlab.md) – running other quality algorithms (ST-RRED, ST-MAD, SpEED-QA, and BRISQUE) with MATLAB
- [BD-rate utilities](usage/bd-rate.md) – Bjontegaard-delta rate and VMAF gain helpers
- [Per-shot scoring](usage/vmaf-perShot.md) – per-shot VMAF scoring workflow
- [ROI scoring](usage/vmaf-roi.md) – region-of-interest saliency-weighted scoring
- [External resources](usage/external-resources.md) – e.g. software using VMAF
- `vmaf-tune` — quality-aware encode automation harness:
  [overview](usage/vmaf-tune.md) |
  [fast path](usage/vmaf-tune-fast-path.md) |
  [bitrate ladder](usage/vmaf-tune-ladder.md) |
  [codec adapters](usage/vmaf-tune-codec-adapters.md) |
  [recommend](usage/vmaf-tune-recommend.md) |
  [saliency-aware](usage/vmaf-tune-saliency-aware.md) |
  [resolution-aware](usage/vmaf-tune-resolution-aware.md) |
  [HDR & sampling](usage/vmaf-tune-hdr-and-sampling.md) |
  [cache](usage/vmaf-tune-cache.md) |
  [bisect](usage/vmaf-tune-bisect.md)

## C API

- [API overview](api/index.md) – core `libvmaf.h` + `picture.h` + `model.h` + `feature.h`: context lifecycle, scoring, pictures, models, ABI-stability tiers, thread-safety, runnable example
- [DNN sessions](api/dnn.md) – tiny-AI `dnn.h`: standalone ONNX sessions (luma filter + multi-input named binding), device config, error codes
- [GPU (CUDA / SYCL)](api/gpu.md) – `libvmaf_cuda.h` + `libvmaf_sycl.h`: zero-copy frame buffers, dmabuf / VA / D3D11 import, profiling
- [MCP C API](api/mcp.md) – `libvmaf_mcp.h`: embedded in-process MCP server (stdio / UDS / SSE transports)

## Metrics

- [Features](metrics/features.md) – VMAF's core features (metrics)
- [CAMBI](metrics/cambi.md) – contrast-aware multiscale banding index
- [SSIMULACRA 2](metrics/ssimulacra2.md) – fork-added structural similarity metric (modern-codec quality)
- [DISTS-Sq](metrics/dists.md) – fork-added deep image structure & texture similarity extractor (FR; smoke checkpoint)
- [Confidence Interval](metrics/confidence-interval.md) – bootstrapping for CI estimates of VMAF scores
- [Bad Cases](metrics/bad-cases.md) – how to report cases of VMAF not working well
- [AOM CTC](metrics/ctc/aom.md) – running VMAF under [AOM](http://aomedia.org/) common test conditions
- [NFLX CTC](metrics/ctc/nflx.md) – running VMAF under NFLX common test conditions

## Models

- [Overview](models/overview.md) – summary of the available pre-trained models
- [Datasets](models/datasets.md) – the two publicly available datasets for training custom models

## Backends

GPU / SIMD backend notes under [backends/](backends/index.md):

| Backend | Status | Page |
|---------|--------|------|
| x86 SIMD (AVX2 / AVX-512) | Production | [avx512.md](backends/x86/avx512.md) |
| ARM NEON / SVE2 | Production | [arm/overview.md](backends/arm/overview.md) |
| CUDA | Production | [cuda/overview.md](backends/cuda/overview.md) + [NVTX profiling](backends/nvtx/profiling.md) |
| SYCL / oneAPI | Production | [sycl/overview.md](backends/sycl/overview.md) + [bundling](backends/sycl/bundling.md) |
| Vulkan | **Removed** ([ADR-0726](adr/0726-drop-vulkan-backend.md), 2026-05-28) | — |
| HIP (AMD ROCm) | 7 of 10 real kernels (psnr, integer_psnr, float_motion, float_moment, float_ssim, ciede, integer_motion_v2); 3 stubs (adm/vif/integer_motion). `float_ansnr_hip` removed per [ADR-0865](adr/0865-ansnr-sunset-pre-vmaf-metric-drop.md). | [hip/overview.md](backends/hip/overview.md) |
| Metal (Apple Silicon) | Scaffold — 4 of 17 extractors registered | [metal/index.md](backends/metal/index.md) |

## Architecture

- [Repository layout](architecture/index.md) – what lives where + decision tree
- [Python-harness workspace](architecture/workspace.md) – the moved `workspace/` tree
- [ADR log](adr/README.md) – every non-trivial architectural / policy decision + rationale

## MCP

- [MCP server overview](mcp/index.md) – install, security model (path allowlist), env vars, Claude Desktop / Cursor config
- [Tool reference](mcp/tools.md) – per-tool request/response schemas + error codes for `vmaf_score`, `list_models`, `list_backends`, `run_benchmark`, `eval_model_on_split`, `compare_models`
- [Embedded server](mcp/embedded.md) – in-process `libvmaf_mcp.h` server (stdio / UDS / SSE); `compute_vmaf` + `list_features` tools live
- [Release channel](mcp/release-channel.md) – PyPI packaging and versioning for the standalone Python server

## Tiny-AI

- [Tiny-AI docs](ai/index.md) – overview, training, inference, benchmarks, security
- [Training](ai/training.md) + [training data](ai/training-data.md) + [MOS corpora](ai/mos-corpora.md)
- [Inference](ai/inference.md) – ONNX Runtime EPs: CPU, CUDA, CoreML, OpenVINO (CPU / GPU / NPU); TensorRT on [roadmap](ai/roadmap.md)
- [LOSO evaluation](ai/loso-eval.md) + [predictor](ai/predictor.md) + [conformal VQA](ai/conformal-vqa.md)
- [Ensemble training kit](ai/ensemble-training-kit.md) + [ensemble v2 runbook](ai/ensemble-v2-real-corpus-retrain-runbook.md)
- [Quantization](ai/quantization.md) + [quant epsilon](ai/quant-eps.md)
- [Model registry](ai/model-registry.md) – canonical registry of all shipped ONNX / JSON models
- [Tiny-AI roadmap](ai/roadmap.md) – Wave 1 scope expansion (LPIPS, saliency, per-shot CRF, `vmaf_post`, allowlist `Loop`/`If`, MCP VLM tool)

## Development

- [Engineering principles](principles.md) – NASA Power-of-10 + JPL + CERT + MISRA, golden gate, quality policy
- [Benchmarks](benchmarks.md) – fork-added benchmark numbers (GPU, SIMD, `--precision`)
- [Build flags](development/build-flags.md) – every `meson_options.txt` option with defaults, effects, and flag interactions
- [Release](development/release.md) – how to perform a new release
- [CI](development/ci.md) – CI pipeline overview + required status checks
- [CI runners](development/ci-runners.md) – self-hosted runner setup for GPU lanes
- [Cross-backend gate](development/cross-backend-gate.md) – T6-8 GPU-parity gate semantics
- [Fuzzing](development/fuzzing.md) – OSS-Fuzz integration
- [IDE setup](development/ide-setup.md) – clangd / VS Code configuration for all backends
- [FFmpeg patches refresh](development/ffmpeg-patches-refresh.md) – rebasing the `ffmpeg-patches/` series
- [Upstream watchers](development/upstream-watchers.md) – tracking Netflix/vmaf divergence
- [Security & Scorecard audit](security/scorecard-alerts-2026-09-04.md) – OpenSSF Scorecard and CodeQL alerts audit

## Reference

- [FAQ](reference/faq.md)
- [References](reference/references.md) – a list of links and papers
- [Papers](https://github.com/VMAFx/vmafx/tree/master/docs/reference/papers)
  and [Presentations](https://github.com/VMAFx/vmafx/tree/master/docs/reference/presentations)
