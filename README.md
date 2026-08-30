# VMAFx

<!-- Build / test / security CI badges -->
[![Tests](https://github.com/VMAFx/vmafx/actions/workflows/tests-and-quality-gates.yml/badge.svg)](https://github.com/VMAFx/vmafx/actions/workflows/tests-and-quality-gates.yml)
[![Lint](https://github.com/VMAFx/vmafx/actions/workflows/lint-and-format.yml/badge.svg)](https://github.com/VMAFx/vmafx/actions/workflows/lint-and-format.yml)
[![Security](https://github.com/VMAFx/vmafx/actions/workflows/security-scans.yml/badge.svg)](https://github.com/VMAFx/vmafx/actions/workflows/security-scans.yml)
[![Builds](https://github.com/VMAFx/vmafx/actions/workflows/libvmaf-build-matrix.yml/badge.svg)](https://github.com/VMAFx/vmafx/actions/workflows/libvmaf-build-matrix.yml)
[![FFmpeg](https://github.com/VMAFx/vmafx/actions/workflows/ffmpeg-integration.yml/badge.svg)](https://github.com/VMAFx/vmafx/actions/workflows/ffmpeg-integration.yml)
[![Rust (CI)](https://github.com/VMAFx/vmafx/actions/workflows/rust-ci.yml/badge.svg)](https://github.com/VMAFx/vmafx/actions/workflows/rust-ci.yml)
[![Go (CI)](https://github.com/VMAFx/vmafx/actions/workflows/go-ci.yml/badge.svg)](https://github.com/VMAFx/vmafx/actions/workflows/go-ci.yml)

<!-- Version / language stack badges -->
[![Go 1.26](https://img.shields.io/badge/Go-1.26-00ADD8?logo=go&logoColor=white)](https://go.dev/doc/go1.26)
[![Rust edition 2024](https://img.shields.io/badge/Rust-edition%202024-CE422B?logo=rust&logoColor=white)](https://doc.rust-lang.org/edition-guide/rust-2024/)
[![Python 3.14+](https://img.shields.io/badge/Python-3.14%2B-3776AB?logo=python&logoColor=white)](https://docs.python.org/3.14/)
[![C11](https://img.shields.io/badge/C-C11-00599C?logo=c&logoColor=white)](https://en.cppreference.com/w/c/11)
[![C++23](https://img.shields.io/badge/C%2B%2B-C%2B%2B23-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![CUDA 13.2](https://img.shields.io/badge/CUDA-13.2-76B900?logo=nvidia&logoColor=white)](https://developer.nvidia.com/cuda-downloads)
[![ROCm 7.2](https://img.shields.io/badge/ROCm-7.2-ED1C24?logo=amd&logoColor=white)](https://rocm.docs.amd.com/)

<!-- GPU / SIMD capability badges -->
[![GPU: CUDA · SYCL · HIP · Metal](https://img.shields.io/badge/GPU-CUDA%20%C2%B7%20SYCL%20%C2%B7%20HIP%20%C2%B7%20Metal-76B900?logo=nvidia&logoColor=white)](docs/backends/)
[![SIMD: AVX2 · AVX-512 · NEON](https://img.shields.io/badge/SIMD-AVX2%20%C2%B7%20AVX--512%20%C2%B7%20NEON-orange?logo=intel&logoColor=white)](docs/backends/)

<!-- Distribution / community badges -->
[![Container](https://img.shields.io/badge/Container-ghcr.io%2Fvmafx%2Fvmafx-2496ED?logo=docker&logoColor=white)](https://github.com/orgs/VMAFx/packages)
[![License: BSD-2-Clause-Patent](https://img.shields.io/badge/License-BSD--2--Clause--Patent-blue.svg)](LICENSE)
[![Conventional Commits](https://img.shields.io/badge/Conventional%20Commits-1.0.0-%23FE5196?logo=conventionalcommits)](https://www.conventionalcommits.org)
[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/VMAFx/vmafx/badge)](https://securityscorecards.dev/viewer/?uri=github.com/VMAFx/vmafx)
[![ko-fi](https://img.shields.io/badge/ko--fi-support%20lusoris-ff5e5b?logo=kofi&logoColor=white)](https://ko-fi.com/lusoris)

**A GPU-accelerated, full-precision, signed-release fork of
[Netflix/vmaf](https://github.com/Netflix/vmaf)** — perceptual video quality
assessment, Emmy-winning, now with:

- **SYCL / oneAPI** GPU backend (Intel, NVIDIA, AMD via Codeplay plugins),
  with a fp64-less device fallback path for Intel Arc / iGPU silicon.
- **CUDA** GPU backend (optimized ADM decouple fusion, VIF rd_stride,
  memory-efficient scoring).
- **HIP (AMD ROCm)** GPU backend — 19 registered feature extractors have real
  device kernels (PSNR, float-PSNR, motion, motion\_v2, moment, SSIM, MS-SSIM,
  CIEDE2000, ADM, VIF, and more — see [backends/hip](docs/backends/hip/overview.md));
  3 legacy API stubs (`adm_hip`, `vif_hip`, `motion_hip`) are not registered.
  `float_ansnr_hip` was removed in PR #38. Requires
  `-Denable_hip=true -Denable_hipcc=true` and ROCm ≥ 7.
- **AVX2 / AVX-512 / NEON** SIMD paths for every hot kernel.
- **`--precision`** CLI flag — default `%.6f` matches upstream Netflix output
  (keeps the CPU golden gate green without per-call flags); `--precision=max`
  opts in to `%.17g` for IEEE-754 round-trip lossless scores. See ADR-0119
  (supersedes ADR-0006).
- **Tiny-AI** model surface (ONNX Runtime) for lightweight quality-proxy
  experiments — Netflix + KoNViD-1k combined-corpus trainer, LOSO eval
  harness, multi-seed validation, QAT + PTQ paths. See [`ai/`](ai/).
- **MCP servers** — both an in-process embedded scaffold
  ([`core/include/libvmaf/libvmaf_mcp.h`](core/include/libvmaf/libvmaf_mcp.h),
  flag `-Denable_mcp=true`) and the standalone Python JSON-RPC server
  under [`mcp-server/vmaf-mcp/`](mcp-server/vmaf-mcp/).
- **GPU-parity CI gate** — every PR runs a CPU ↔ GPU variance matrix across
  all features; CUDA / SYCL / HIP join when a self-hosted runner is
  registered. See
  [`docs/development/cross-backend-gate.md`](docs/development/cross-backend-gate.md).
- **Signed releases** — every tag carries SBOM (SPDX + CycloneDX), Sigstore
  keyless signatures, and SLSA L3 provenance.

Upstream Netflix/vmaf stays authoritative for the scoring algorithm; the fork
adds backends, tooling, and productization without changing the numerical
contract. The three Netflix CPU golden-data tests (1 normal + 2 checkerboard
pairs) run as a required CI gate on every PR — see
[`docs/principles.md`](docs/principles.md) §3.1 and decision D24.

![vmaf logo](python/vmaf/resource/images/vmaf_logo.jpg)

## Quickstart

```bash
# One-liner dev env install (auto-detects Ubuntu/Arch/Fedora/Alpine/macOS/Win).
./scripts/setup/detect.sh

# CPU-only build + test. The build root is core/ (ADR-0700).
meson setup build core -Denable_cuda=false -Denable_sycl=false
ninja -C build
meson test -C build

# Score a pair.
build/tools/vmaf -r ref.yuv -d dis.yuv --width 1920 --height 1080 \
                 -p 420 -b 8 -m version=vmaf_v0.6.1 --precision=17
```

Add `-Denable_cuda=true` (requires `/opt/cuda`), `-Denable_sycl=true`
(requires oneAPI `icpx`), or `-Denable_hip=true -Denable_hipcc=true`
(requires ROCm ≥ 7 + `hipcc`) to bring up a GPU backend. The embedded
MCP server lands behind `-Denable_mcp=true` (scaffold currently returns
`-ENOSYS`; transports in T5-2b).

## Backends at a glance

<!-- markdownlint-disable MD060 -->
| Backend | Status | Notes |
| ------- | ------ | ----- |
| CPU     | ✅     | Scalar + AVX2 + AVX-512 + NEON. Golden-data truth. |
| CUDA    | ✅     | `/opt/cuda`, `nvcc`. Works on RTX 20xx and newer. `CU_STREAM_NON_BLOCKING` motion speedup (PR #702). |
| SYCL    | ✅     | oneAPI DPC++; Intel/NVIDIA/AMD via Codeplay; fp64-less device fallback for Arc / iGPU. |
| HIP     | 🔶     | 19 registered kernels (`-Denable_hip=true -Denable_hipcc=true`); 3 legacy API stubs not registered; `float_ansnr_hip` removed PR #38. |
| Metal   | 💭     | Apple Silicon scaffold (8/17 real); `-Denable_metal=auto/enabled`; not prioritized, PRs welcome. |
<!-- markdownlint-enable MD060 -->

Cross-backend numerical divergence is held to ≤ 2 ULP in double precision; see
[`/cross-backend-diff`](.claude/skills/cross-backend-diff/SKILL.md) for the
verification loop.

**FFmpeg integration:** patches against `n8.1.1` cover the supported GPU
backends and the DNN/tiny-model surface. Configure flags:
`--enable-libvmaf-{cuda,sycl,hip}`. See
[`ffmpeg-patches/`](ffmpeg-patches/).

**Symbol visibility (PR #706,
[ADR-0379](docs/adr/0379-libvmaf-symbol-visibility.md)):**
`libvmaf.so` exports exactly **44 `vmaf_*` public symbols** — zero
leaked internal symbols (was 207 leaked, including libsvm, pdjson,
and SIMD kernel names).

**Compiler support:** GCC 16 is supported (PR #699).

## CLI additions (fork-only)

```text
--precision $spec
      score output precision
        N (1..17) -> printf "%.<N>g"
        max|full  -> "%.17g" (IEEE-754 round-trip lossless; opt-in)
        legacy    -> "%.6f" (default; matches upstream Netflix output)

--backend $name            auto|cpu|cuda|sycl|hip|metal (auto-selects if omitted)
--no_cuda                  disable CUDA backend
--no_sycl                  disable SYCL/oneAPI backend
--sycl_device $unsigned    select SYCL GPU by index (default: auto)
--no_hip                   disable HIP (AMD ROCm) backend
--hip_device $unsigned     select HIP GPU by index (default: auto)
--no_metal                 disable Metal (Apple Silicon) backend
--metal_device $unsigned   select Metal GPU by index (default: auto)
--gpumask: $bitmask        restrict permitted GPU operations

--tiny-model $path         load a tiny ONNX model alongside classic models
--tiny-device $string      auto|cpu|cuda|openvino|rocm (default: auto)
--tiny-threads $unsigned   CPU EP intra-op threads (0 = ORT default)
--tiny-fp16                request fp16 IO where the EP supports it
```

All upstream flags are preserved unchanged.

## Tiny AI

Lightweight perceptual-quality models trained and shipped in-repo, consumed
through a single ONNX Runtime-backed inference path inside libvmaf.

| # | Capability | What it is | Where it runs |
| --- | --- | --- | --- |
| C1 | **Custom FR models** | Tiny MLP regressor on the libvmaf feature vector → MOS. Drop-in for the upstream SVM. | libvmaf, `vmaf` CLI, ffmpeg `libvmaf` filter |
| C2 | **No-reference metrics** | Small CNN / MobileNet-tiny on the distorted frame alone. | libvmaf, `vmaf --no-reference`, ffmpeg filter |
| C3 | **Learned filters** | Residual CNN denoisers / sharpeners exposed through ffmpeg `vmaf_pre`. | ffmpeg `vmaf_pre`, `dnn_processing` |
| C4 | **LLM dev helpers** | Ollama-backed review / commit-msg / docgen helpers, never linked into libvmaf. | [`dev-llm/`](dev-llm/), `.claude/skills/dev-llm-*` |

- Training: [`ai/`](ai/) (`pip install -e ai && vmaf-train --help`).
- Inference runtime: [`core/src/dnn/`](core/src/dnn/) (C, ONNX Runtime).
- CLI usage: `vmaf --tiny-model model/tiny/vmaf_tiny_fr_v1.onnx [--tiny-device cuda]`.
- Meson flag: `-Denable_dnn=auto|enabled|disabled` (default `auto`).
- ffmpeg: apply [`ffmpeg-patches/*.patch`](ffmpeg-patches/) for
  `tiny_model=...` and the new `vmaf_pre` filter.
- Docs: [`docs/ai/`](docs/ai/).

## Documentation

- [`CLAUDE.md`](CLAUDE.md) — orientation for Claude Code sessions.
- [`AGENTS.md`](AGENTS.md) — same, for tool-agnostic agents
  (Cursor, Aider, Copilot).
- [`docs/principles.md`](docs/principles.md) — NASA Power-of-10, JPL, CERT,
  MISRA coding standard, Netflix golden gate, quality policy.
- [`docs/backends/`](docs/backends/) — per-backend overviews (CUDA, SYCL,
  HIP, x86, arm); SYCL bundling at
  [`docs/backends/sycl/bundling.md`](docs/backends/sycl/bundling.md).
- [`docs/development/cross-backend-gate.md`](docs/development/cross-backend-gate.md)
  — GPU-parity matrix CI gate (T6-8).
- [`docs/benchmarks.md`](docs/benchmarks.md) — fork-added benchmark numbers
  (GPU, SIMD, `--precision`).
- [`docs/ai/`](docs/ai/) — training, inference, LOSO eval, QAT/PTQ,
  benchmarks, security.
- [`docs/mcp/`](docs/mcp/) — embedded + standalone MCP server docs.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — how to contribute (fork-specific +
  upstream guide preserved).
- [`SECURITY.md`](SECURITY.md) — coordinated disclosure, SLA, supply-chain
  guarantees.
- [Netflix/vmaf upstream docs](docs/index.md) — FAQs, models, AOM CTC usage.

## Release & signing

Tagged releases use one ordinary SemVer stream (`vMAJOR.MINOR.PATCH`), owned
by VMAFx and independent of upstream Netflix's release cadence. Every release
asset is:

- Signed with [Sigstore](https://sigstore.dev) keyless OIDC — verify with
  `cosign verify-blob --bundle <asset>.bundle <asset>`.
- Accompanied by SPDX and CycloneDX SBOMs.
- Backed by [SLSA L3](https://slsa.dev) provenance via
  `slsa-github-generator` — verify with `slsa-verifier`.

Release automation: [release-please](https://github.com/googleapis/release-please)
opens a PR on pushes to `master`. Merging it creates a draft release; publishing
that draft creates the tag and starts signing and container publication.

## License

[BSD-2-Clause-Patent](LICENSE) — preserved from upstream Netflix/vmaf.

Fork-authored code (SYCL backend, `.claude/` scaffolding, MCP server, Tiny-AI
surface) is © 2024-2026 Lusoris, licensed under the
same BSD-2-Clause-Patent terms as the rest of the project.

## Attribution

Upstream: [Netflix/vmaf](https://github.com/Netflix/vmaf). The scoring
algorithm, Python training harness, and the 3 Netflix CPU golden test pairs
remain Netflix's. The fork wraps, extends, and hardens — it does not replace.

Fork maintainers: [Lusoris](https://github.com/Lusoris) and
[Claude (Anthropic)](https://www.anthropic.com/claude) — co-authored.

---

## Support the fork

If the fork saves you time, [ko-fi.com/lusoris](https://ko-fi.com/lusoris)
keeps the GPU bill paid and the test rigs running.

## Upstream news & history

See [`CHANGELOG.md`](CHANGELOG.md) for fork-specific changes and the
[upstream release history](https://github.com/Netflix/vmaf/releases) for the
core VMAF algorithm evolution (CAMBI, NEG mode, v3.0.0 API overhaul, etc.).
