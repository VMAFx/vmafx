<!-- markdownlint-disable MD013 MD060 -->
# Architecture

Map of the repository, from the top down.

## Repository layout

```text
vmaf/
├── core/               # The C library + CLI. The product. (was libvmaf/, ADR-0700)
│   ├── src/            # metric engine, feature extractors
│   │   ├── feature/    # per-feature CPU kernels
│   │   │   ├── x86/    # AVX2 / AVX-512 SIMD paths
│   │   │   ├── arm64/  # NEON SIMD paths
│   │   │   ├── cuda/   # CUDA kernels
│   │   │   └── sycl/   # SYCL kernels
│   │   ├── cuda/       # CUDA backend runtime (picture, dispatch)
│   │   ├── sycl/       # SYCL backend runtime (queue, USM, dmabuf)
│   │   └── dnn/        # ONNX Runtime integration (Tiny-AI, docs/ai/)
│   ├── include/        # public C API headers
│   ├── tools/          # `vmaf` CLI, `vmaf_bench` benchmark driver
│   └── test/           # libvmaf C unit tests
│
├── ai/                 # Fork: Tiny-AI training harness (torch + lightning)
│   └── src/vmaf_train/ # typer CLI → ONNX artefacts under model/tiny/
│
├── model/              # Shipped VMAF models (vmaf_v0.6.1.json, .pkl)
│   └── tiny/           # Fork: ONNX tiny models (C1/C2/C3 from ai/)
│
├── python/             # Python bindings + classic training harness
│   └── vmaf/
│       ├── core/       # VmafRunner, feature assemblers
│       ├── script/     # run_vmaf, run_testing, run_vmaf_training
│       └── workspace/  # Python-harness scratch (see workspace.md)
│
├── mcp-server/         # Fork: MCP JSON-RPC server (Python)
│
├── testdata/           # YUV fixtures + fork benchmark JSONs
│
├── docs/               # All documentation (this tree)
│   ├── architecture/   # <-- you are here
│   ├── ai/             # Tiny-AI: train / infer / bench / security
│   ├── backends/       # CPU / CUDA / SYCL / HIP backend notes
│   ├── metrics/        # Per-metric (VMAF, SSIM, MS-SSIM, CAMBI, ...)
│   ├── models/         # Model files, training overview
│   ├── usage/          # CLI, Python, FFmpeg, Docker
│   ├── development/    # Releases, contributor workflow
│   ├── reference/      # Papers, presentations, FAQ
│   └── getting-started # Installation, first build
│
├── .claude/            # Claude Code agent config (skills, hooks, agents)
├── .workingdir2/       # Planning dossier (read-only at runtime)
└── .github/workflows/  # CI / release / supply chain
```

## What lives where (decision tree)

| Concern                                | Home                                          |
| -------------------------------------- | --------------------------------------------- |
| Add a SIMD path                        | `core/src/feature/<isa>/`                  |
| Add a GPU backend                      | `core/src/<backend>/` + `core/src/feature/<backend>/` |
| Add a feature extractor                | `core/src/feature/`                        |
| Ship a new VMAF model                  | `model/` (JSON/pkl) or `model/tiny/` (ONNX)   |
| Train a new tiny model                 | `ai/src/vmaf_train/models/`                   |
| Python harness scratch                 | `python/vmaf/workspace/` (see [workspace.md](workspace.md)) |
| CI / release workflow                  | `.github/workflows/`                          |
| Coding standards / style               | [`docs/principles.md`](../principles.md)      |
| Planning artefacts (design docs)       | `.workingdir2/` (checked in)                  |

## C4 model

The fork uses the [C4 model](https://c4model.com) for top-down architecture
views. Levels are scaffolded and evolve as internal boundaries stabilise:

- **[c4-context.md](c4-context.md)** — Level 1: system context (users,
  external systems).
- **[c4-container.md](c4-container.md)** — Level 2: containers (libvmaf,
  vmaf CLI, ai/, mcp-server/, …).
- *Level 3 (component) added per-container as needed.*
- *Level 4 (code) generated on demand — not hand-maintained.*

## Related reading

- **[workspace.md](workspace.md)** — the Python harness scratch tree (and why it moved).
- **[grpc-streaming.md](grpc-streaming.md)** — vmafx-server `ScoreStream` bidirectional RPC (ADR-0933, Phase 1).
- **[../principles.md](../principles.md)** — coding standards (NASA/JPL, CERT C).
- **[../ai/overview.md](../ai/overview.md)** — Tiny-AI architecture (C1 / C2 / C3 / C4).
- **[../backends/](../backends/)** — CPU / CUDA / SYCL backend internals.
- **[../adr/README.md](../adr/README.md)** — Architectural Decision Records (ADRs).
