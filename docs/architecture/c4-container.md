# C4 Level 2 — Container view

This page lists the deployable and buildable units of VMAFx (the containers
in C4 terms), how they depend on each other, and the rules that keep their
boundaries stable. Read the diagram first, then the table for the directory
that owns each container. [c4-context.md](c4-context.md) is Level 1 and
[index.md](index.md) is the on-disk repository map.

!!! note "Status"
    This is a living overview, not a complete component model. It was
    scaffolded on 2026-04-17 and now covers the C engine, the tiny-AI
    stack, the MCP surfaces and the Go platform binaries. Level 3 (one
    diagram per container) is not written yet.

## Container diagram

The containers are written as a table of relations; each container is
described under [Containers](#containers). `model/` and `testdata/` are file
stores; ONNX Runtime and GitHub are outside the VMAFx boundary.

| From | To | How |
| --- | --- | --- |
| Video engineer | vmaf + vmaf_bench | Runs vmaf --tiny-model ... ref.yuv dist.yuv |
| Video engineer | mcp-server/vmaf-mcp | Connects over JSON-RPC from an MCP-capable client |
| Video engineer | cmd/vmafx-mcp | Connects over JSON-RPC from an MCP-capable client |
| Video engineer | cmd/vmafx-server, vmafx-controller, | Submits scoring jobs over gRPC / REST |
| Coding agent | ai/ | Trains / exports new tiny models |
| vmaf + vmaf_bench | libvmaf | links |
| libvmaf | core/src/dnn | opens vmaf_dnn_session_* when a tiny model is loaded |
| core/src/dnn | ONNX Runtime | C API: CreateSession, Run |
| core/src/dnn | model/ | Reads .onnx + registry.json; verifies sha256 |
| ai/ | model/ | Writes .onnx checkpoints + registry entries |
| mcp-server/vmaf-mcp | vmaf + vmaf_bench | vmaf CLI subprocess |
| cmd/vmafx-mcp | vmaf + vmaf_bench | vmaf CLI subprocess, optional cgo scoring, gRPC for the 5 job tools |
| cmd/vmafx-mcp | cmd/vmafx-server, vmafx-controller, | gRPC |
| core/src/mcp | libvmaf | in-process |
| cmd/vmafx-server, vmafx-controller, | vmaf + vmaf_bench | vmaf CLI subprocess |
| compat/python-vmaf | libvmaf | Bindings for classic harness |
| vmaf + vmaf_bench | testdata/ | Reads fixtures for benchmarks |
| GitHub | testdata/ | CI validates snapshot JSONs against backends |

## Containers

| Container | Language | Responsibility | AGENTS.md |
| --- | --- | --- | --- |
| libvmaf | C11 | Metric engine, feature extractors, backend dispatch, public API | [../../core/AGENTS.md](../../core/AGENTS.md) |
| core/src/dnn | C11 | Tiny-AI inference layer (loader + op-allowlist + ORT session) | [../../core/src/dnn/AGENTS.md](../../core/src/dnn/AGENTS.md) |
| core/src/feature | C11 + SIMD + CUDA + SYCL + HIP + Metal | Per-feature scalar + vector + GPU kernels | [../../core/src/feature/AGENTS.md](../../core/src/feature/AGENTS.md) |
| core/src/cuda | C + CUDA | CUDA backend runtime (picture, stream, ring buffer) | [../../core/src/cuda/AGENTS.md](../../core/src/cuda/AGENTS.md) |
| core/src/sycl | C++ + SYCL/DPC++ | SYCL backend runtime (USM, dmabuf) | [../../core/src/sycl/AGENTS.md](../../core/src/sycl/AGENTS.md) |
| core/src/hip, core/src/metal | C + HIP, Objective-C++ + Metal | HIP and Metal backend runtimes | [../../core/src/AGENTS.md](../../core/src/AGENTS.md) |
| core/src/mcp | C11 | Embedded MCP server inside libvmaf (`-Denable_mcp`) | [../../core/src/mcp/AGENTS.md](../../core/src/mcp/AGENTS.md) |
| core/tools | C11 + C++ | `vmaf`, `vmaf_bench`, `vmaf_per_shot`, `vmaf_roi`, `vmaf_vpl` binaries | [../../core/tools/AGENTS.md](../../core/tools/AGENTS.md) |
| core/test | C11 | C unit tests (µnit-style) | [../../core/test/AGENTS.md](../../core/test/AGENTS.md) |
| ai/ | Python + PyTorch + Lightning | Tiny-AI training + ONNX export (`vmaf-train` CLI) | [../../ai/AGENTS.md](../../ai/AGENTS.md) |
| mcp-server/vmaf-mcp | Python | MCP tool surface, 19 tools (see [mcp/tools.md](../mcp/tools.md)) | [../../mcp-server/AGENTS.md](../../mcp-server/AGENTS.md) |
| cmd/vmafx-mcp | Go | Recommended MCP server, 24 tools: the 19 shared tools plus 5 control-plane tools | [../../cmd/AGENTS.md](../../cmd/AGENTS.md) |
| cmd/vmafx-server, vmafx-controller, vmafx-node, vmafx-operator | Go | Scoring service, job controller, worker node, Kubernetes operator (see [phase4b-distributed-platform.md](phase4b-distributed-platform.md)) | [../../cmd/AGENTS.md](../../cmd/AGENTS.md) |
| cmd/vmafx-tune, cmd/vmafx-ort-runner | Go | Encoder-tuning CLI; one-shot ONNX Runtime subprocess | [../../cmd/AGENTS.md](../../cmd/AGENTS.md) |
| pkg/, proto/, deploy/helm/vmafx | Go, protobuf, YAML | Shared Go packages, gRPC contracts, Helm chart and CRDs | [../../cmd/AGENTS.md](../../cmd/AGENTS.md) |
| compat/python-vmaf | Python | Classic SVM harness + bindings; `python/` re-exports it and holds the golden-data tests in `python/test/` | [Python compatibility invariants](https://github.com/vidiko/vmafx/blob/master/compat/python-vmaf/AGENTS.md) |
| model/ | Files | Shipped models (`.json`, `.pkl`, `.onnx` + registry.json) | n/a |
| testdata/ | Files | YUV fixtures + fork benchmark JSONs | n/a |

## Boundary invariants

1. **libvmaf is C-only** on the runtime path. Python and PyTorch are
   training-only and never linked into the shipped library.
2. **Training and runtime meet on disk.** The boundary is `.onnx` plus a
   sidecar JSON file; `ai/` and `core/src/dnn/` communicate only through
   files in `model/tiny/`. See
   [ADR-0021](../adr/0021-training-stack-pytorch-lightning.md) and
   [ADR-0022](../adr/0022-inference-runtime-onnx.md).
3. **Untrusted ONNX input is scanned first.** Every `.onnx` loaded via
   `--tiny-model` is checked for banned ops before `CreateSession` is
   called. See [ADR-0039](../adr/0039-onnx-runtime-op-walk-registry.md).
4. **Backend selection is per invocation.** The CPU, CUDA, SYCL, HIP and
   Metal backends are chosen at run time (`--backend`) among those compiled
   into the binary. Each backend is enabled at build time on its own
   (`enable_cuda`, `enable_sycl`, `enable_hip`); not all of them have to be
   built.

## Next levels

- **Level 3 — Component**: one diagram per container showing its internal
  modules. Add as components stabilise (starting with core/src/dnn since
  that is the newest, most active boundary).
- **Level 4 — Code**: generated on demand via `ctags` / clang AST, not
  hand-maintained.
