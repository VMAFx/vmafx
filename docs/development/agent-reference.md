<!-- markdownlint-disable MD013 -->
# Agent reference

Reference material moved out of root [AGENTS.md](../../AGENTS.md) so always-loaded agent context stays under client caps. This document preserves detailed operational guidance on repository layout, workflow skills, golden-data verification, IDE configuration, and release engineering stages.

## Repository layout

The repository layout organizes components across the following primary directories (formerly documented in `CLAUDE.md` §5):

- `core/` — C library and build root (formerly `libvmaf/`, ADR-0700); public C API headers reside in `core/include/libvmaf/`, and C unit tests in `core/test/`.
- `core/src/` — C engine: metric calculation engine and feature extractors.
- `core/src/{cuda,sycl,dnn}/` — GPU and DNN backends: CUDA runtime (picture, dispatch), SYCL runtime (queue, USM, dmabuf), and ONNX Runtime integration.
- `core/src/feature/{x86,arm64,cuda,sycl}/` — Platform-specific feature implementations: AVX2 / AVX-512, NEON, CUDA kernels, and SYCL kernels.
- `core/tools/` — CLI tools, including the `vmaf` executable (`vmaf.cpp`, `cli_parse.cpp`, `vmafx` symlink / Windows exe) and `vmaf_bench`.
- `compat/python-vmaf/` — Python harness package (formerly `python/vmaf/`, ADR-0700).
- `python/vmaf/` + `python/test/` — Python compatibility shim re-exporting `compat/python-vmaf/` along with test suites containing the Netflix golden-data assertions.
- `ai/` — PyTorch and Lightning tiny-model training infrastructure.
- `mcp-server/` — Model Context Protocol JSON-RPC server (`mcp-server/vmaf-mcp/`, Python).
- `model/` — VMAF models in `.json`, `.pkl`, and `.onnx` formats.
- `testdata/` — Fork-added reference YUV sequences and snapshot JSONs.
- `dev/` — Development container environment (`Containerfile`, `docker-compose.yml`, `scripts/`).
- `docs/` — Documentation tree, including upstream documentation and fork extensions; [docs/principles.md](../principles.md) establishes canonical engineering standards.
- `.claude/` — Claude Code configuration containing skills, agents, and hooks.
- `.workingdir/` — Private session state, bounded cache, local evidence, and recovery material (gitignored); never use it as public documentation authority.
- `.corpus/` — Local datasets, extracted media, reusable encodes, and derived feature tables (gitignored); do not mix corpora into `.workingdir/`.

Public documentation links only to tracked files. It may show either local root as a command example, but must never hyperlink into an ignored directory.

## Skills for common tasks

Each workflow is provided as a Claude skill under `.claude/skills/`. Agents operating in environments without automated slash-command routing can inspect the matching `SKILL.md` file and execute the workflow steps manually.

| Task | See |
| --- | --- |
| Build against a backend | `.claude/skills/build-vmaf/SKILL.md` |
| Build ffmpeg w/ libvmaf patches | `.claude/skills/build-ffmpeg-with-vmaf/SKILL.md` |
| Refresh ffmpeg-patches/ series | `.claude/skills/refresh-ffmpeg-patches/SKILL.md` |
| Add new GPU backend | `.claude/skills/add-gpu-backend/SKILL.md` |
| Add SIMD path | `.claude/skills/add-simd-path/SKILL.md` |
| Add feature extractor | `.claude/skills/add-feature-extractor/SKILL.md` |
| Add a model | `.claude/skills/add-model/SKILL.md` |
| Add/audit AI run manifest | `.claude/skills/ai-run-manifest/SKILL.md` |
| Cross-backend numeric diff | `.claude/skills/cross-backend-diff/SKILL.md` |
| Validate scores (per-backend ULP) | `.claude/skills/validate-scores/SKILL.md` |
| Run Netflix bench harness | `.claude/skills/run-netflix-bench/SKILL.md` |
| Profile a hot path | `.claude/skills/profile-hotpath/SKILL.md` |
| Bisect a code regression | `.claude/skills/bisect-regression/SKILL.md` |
| Bisect ONNX checkpoint quality | `.claude/skills/bisect-model-quality/SKILL.md` |
| Port upstream commit | `.claude/skills/port-upstream-commit/SKILL.md` |
| Sync with upstream master | `.claude/skills/sync-upstream/SKILL.md` |
| Regenerate test snapshots | `.claude/skills/regen-snapshots/SKILL.md` |
| Regenerate Doxygen / Sphinx docs | `.claude/skills/regen-docs/SKILL.md` |
| Format all (clang-format / black) | `.claude/skills/format-all/SKILL.md` |
| Lint all (clang-tidy / cppcheck) | `.claude/skills/lint-all/SKILL.md` |
| Local-LLM commit-msg draft | `.claude/skills/dev-llm-commitmsg/SKILL.md` |
| Local-LLM Doxygen docgen | `.claude/skills/dev-llm-docgen/SKILL.md` |
| Local-LLM tiny-AI model card | `.claude/skills/dev-llm-modelcard/SKILL.md` |
| Local-LLM file review | `.claude/skills/dev-llm-review/SKILL.md` |
| Release dry-run | `.claude/skills/prep-release/SKILL.md` |

## Netflix golden-data gate

Three Netflix-authored CPU reference test pairs establish numerical-correctness ground truth:

1. **Normal** — `src01_hrc00_576x324.yuv` ↔ `src01_hrc01_576x324.yuv`
2. **Checkerboard 1-px** — `checkerboard_1920_1080_10_3_0_0.yuv` ↔ `..._1_0.yuv`
3. **Checkerboard 10-px** — `checkerboard_1920_1080_10_3_0_0.yuv` ↔ `..._10_0.yuv`

Key requirements and operational details:

- **Reference assets**: YUV reference video files reside in `python/test/resource/yuv/`.
- **Golden-score assertions**: Numerical assertions are hardcoded as `assertAlmostEqual(...)` calls in `python/test/` (`quality_runner_test.py`, `vmafexec_test.py`, `vmafexec_feature_extractor_test.py`, `feature_extractor_test.py`, `result_test.py`).
- **Never modified by any PR**: These assertions are never modified by any pull request except for verbatim ports of upstream Netflix updates ([ADR-1828](../adr/1828-port-netflix-golden-updates.md)). Developers must measure against the fork CPU build first and port the exact upstream value and decimal places without inventing fork-specific numbers. This gate runs in CI as a required status check.
- **Isolated build profile ([ADR-1317](../adr/1317-golden-gate-build-isolation.md))**: `make test-netflix-golden` uses dedicated `GOLDEN_BUILD_DIR ?= core/build-golden` compiled via `scripts/ci/setup-golden-build.sh` enforcing `gcc` or `clang` and passes `VMAF_BUILD_DIR` to Python tests. This isolates the gate from developer builds configured with oneAPI ICX, which exhibit floating-point contraction drift on float-motion and float-VIF convolutions.
- **Fork-added tests**: All fork-specific tests reside in separate files and directories.
- **HDR model score pins**: HDR model score pins run in `python/test/hdr_models_pin_test.py` and `python/test/vmaf_v1_quality_runner_test.py` as part of the golden gate and CI coverage gate. The build requires zimg support (`-Denable_zimg=true`, `libzimg-dev`); any build without zimg fails explicitly naming zimg rather than skipping tests, and CI strictly verifies that no required HDR test is skipped or absent.

## Build and IDE notes

- **IDE configuration**: Both clangd and the VS Code C/C++ extension read `${workspaceFolder}/build/` ([`.vscode/c_cpp_properties.json`](../../.vscode/c_cpp_properties.json)). Configure the build directory with every backend toolchain present; otherwise `compile_commands.json` will lack CUDA and SYCL include paths, producing "undeclared identifier" diagnostics across backend files. For detailed setup steps, see [ide-setup.md](ide-setup.md).
- **Vulkan removal**: Vulkan was removed in [ADR-0726](../adr/0726-drop-vulkan-backend.md). As a result, `volk.h` and `vk_mem_alloc.h` are no longer needed.

## Release stages

The version each stage ships in follows the release map in [release.md](release.md#release-map), which wins over any version named here.

- `release-please`, triggered by pushes to `master`.
- One ordinary SemVer stream (`vMAJOR.MINOR.PATCH`), independent of Netflix release cadence (ADR-1127 supersedes ADR-0011). Root release aligns release-owned Python packages + Helm `appVersion`.
- Signing: keyless, Sigstore / GitHub OIDC. Local dry-run: `/prep-release`.
- Release line restarts at `0.1.0` (ADR-3292, 2026-10-12): `v1.0.0-rc.1`..`rc.3` withdrawn; first release `0.1.0` via one-shot `release-as` (removed in release-merge PR; `scripts/release/check-one-shot-fields.sh` gate); then plain `0.x` minors, no candidates, breaking = minor bump (`bump-minor-pre-major`); candidates only for `1.0.0`, from `v1.0.0-rc.4`. SONAMEs independent of product version. Go: root `go.mod` retracts rc.1-rc.3; proxy `@latest` still v3.0.0+incompatible (Netflix cache) -> consumers pin `@v0.1.0`.
- Stage content, stage names kept (ADR-1341; candidate map ADR-1421, numbering from RC7 on replaced by ADR-1490, scope folded 2026-10-05 by ADR-1868, format envelope + device targets ADR-1880, cloud-native + GStreamer + post-1.0 map ADR-2001; supersedes ADR-1352 map): RC1 (withdrawn rc.1) = release-blocking correctness + outside-hardware report path; RC2 (withdrawn rc.2) = stabilisation/repair (dependency + fix train, RC1 exit bar, no benchmark claim); RC3 (withdrawn rc.3, carry-over 0.2.0) = twin exactness (every GPU/SIMD twin bit-identical to CPU extractor or measured tolerance + ADR, 8K cells included; no SYCL kernel uses scratch memory; integer-overflow audit every extractor + twin at 8K/16K x 16-bit; #1721); RC4 = first full Rust metric (whole `vmaf_v1.0.16_3d0h` path bit-identical to C, C ABI unchanged, GPU twins stay CUDA/SYCL/HIP) + whole device-memory import API (ADR-1829: fences both directions on `VmafPicture2` for CUDA/SYCL/HIP/Metal, NV12/P010 on GPU, CUDA no device-to-device copy, SYCL chroma + D3D11, Metal IOSurface/MTLTexture true binding, HIP import, FFmpeg filters take hardware frames; exit = imported frame bit-identical to host upload, no host pixel copy, fence tests; #1723) + new VMAFx C API, all surfaces generated from `core/api/vmafx.toml`, `libvmaf.h` = thin compat lib, VMAFx-named FFmpeg filters (ADR-1852) + provenance every score (#2142); RC5 = deduplication, one implementation per behaviour, `libgpudispatch` extracted, folds RC4 per-backend import code (#1724, #1455) + tool consolidation (#1249, #1250, #1270, #1272) + new metrics, exact twins written once on `libgpudispatch` (#1247, #1248, #2161, #2158) + Metal SpEED twins (#2160) + device-targeted scoring (ADR-1880); RC6 = GPU capability table generated from `nvcc`/`ocloc`/ROCm `llc`, checked in, CI drift check, all-target kernel audit, RC5 twins included, format envelope per backend/device (max resolution incl. 16K, measured memory limits, tiling where needed, 8-16 bit, chroma layouts, odd + portrait sizes; every row test-backed, drift-checked) (#1725); RC7 = CPU capability source of truth, twin of RC6 (generated table of CPU features each SIMD kernel needs from `core/src/meson.build` flags + `cpu.c` gates, CI drift check; per-function disassembly audit x86 + aarch64; every dispatch level bit-exact vs scalar under Intel SDE models + qemu aarch64 NEON/SVE2 multi-VL; real Xeon/Apple reports = extra evidence; no timing; CPU format envelope, same rules as RC6) (#1885); RC8 = benchmarks, profiling, tuning, throughput per resolution incl. 16K (#1245) + training readiness: data hygiene (#2163, #2157), eval tooling (#2162, #2143), HDR conversion workflow (#2145), mini retrain + measured resource plan (#1246); RC9 = one-shot real retrain + remaining tiny-AI training (#1246, #1242); starts only when every #1246 precondition holds (all above). Cloud-native in 1.0.0 (ADR-2001, no new RC numbers). RC4: versioned scoring API contract (#2155) + server mode + observability (#1251), generated from same definition (WP8). RC4: native `vmafx` GStreamer element (#2236) + CI conformance of upstream gst-plugins-bad `vmaf` element on compat `libvmaf.so.3` (#2237). RC4: OBS-ready API, texture import incl. OpenGL interop, async window scores, bounded queues (#2238); OBS plugin = 1.1. RC4: real-time FFmpeg GPU scoring with `n_stats` (#2138, WP9). ADR-2342 (2026-10-07, no new numbers). RC4 adds: WP7 bindings (Rust/Go/Python generated). WP10 FFmpeg series redesigned from 0001 (#2381 folded). WP11 docs. WP12 packaging (`libvmafx`, Linux arm64, older glibc, package managers #2314/#2319). WP13 input formats (semi-planar/packed, RGB with stated matrix, 8-16 bit). WP14 principles per language. WP15 FFmpeg audit. WP16 observability (#2430). WP17 cloud-native platform (#2431; split with #1252/#1253 drawn by its ADR). Incremental motion. Vulkan import. RC5 adds: tool consolidation in RC5, not after 1.0.0 (Go sunset #1249). Live alignment + timecode (#2354). Interlaced (#2361). Masks (#2362). Container input (#2363). bpp + BD-rate (#2284). HandBrake (#2407). Minimal run-result timeline. Reusable provenance workflow. Vulkan compute experiment. Go saliency via core MobileSal extractor + RC4 Go binding (ADR-2377). RC7 adds: reference conformance column. Default reference-exact. Netflix behaviour = named compat mode, golden gate runs in it. v1 models read compat features until RC9 (ADR-2343). RC5: containers/Helm/kind+kuttl (#1252) + operator, controller/node, GPU pool arbiter in `libgpudispatch` (#1253). RC8: distributed throughput. RC7 = CPU table, drift check, disassembly audit, emulated matrix for kernels that exist; no new-ISA kernel in C (Q-223). New-ISA kernels (x86-64 SSE2/SSSE3/SSE4.1/AVX/AVX2/AVX-512/AVX-512 ICL/AVX10; AArch64 NEON/dotprod+i8mm/SVE/SVE2; RISC-V RVV 1.0; POWER VSX; LoongArch LSX/LASX) written once in Rust, Rust core P1b (#2574); ISA without stable Rust intrinsics = scalar Rust + re-evaluation list (Q-235). qemu-user CI where no hardware; golden gate on each. RC6 = legacy GPU build variants: CUDA 12.x for sm_50-sm_72 (CUDA 13.4 minimum compute_75), Intel legacy compute runtime Gen9-Gen11, every AMD target pinned ROCm compiler emits; bit-exact, in GPU table. RC5 device targets: profiles phone/tablet/laptop/TV/VR per eye incl. portrait; target resolution, scaling, viewing distance/display height mapped on `nvd`/`rdh`. One decode, many targets; short research first; profile table from RC6/RC7 table machinery. Benchmarks/tuning never in RC3-RC7; training never before RC8 evidence accepted; speed lost to exactness in RC3 never traded for tolerance: GPU tuning rows recover in RC8; CPU tuning rows recover in Rust SIMD phases P1a (#2573) / P1b (#2574), measured targets = acceptance (Q-224). Short correctness/device smoke valid in RC1/RC2; performance claims + real training not. Stage-to-release mapping above wins over any release number in the stage ADRs.
- Rust core epic #2567 (ADR-2478; Q-210, Q-230, Q-231): phases P1a 0.3.0, P1b 0.5.0, P2 0.6.0, P3 1.0.0, P4+P5 1.1.0; 1.2.0 host C/C++ removed.
- “Done fixing” for RC1/RC2 = zero confirmed release blockers, zero untriaged `docs/state.md` rows ([docs/state.md](../state.md)), required checks green on exact candidate head, reproducible tester report path. Not: future bugs impossible; not: RC3-RC9 work moved earlier.
- Ordinary Renovate/version PRs remain mergeable in every release phase under normal required checks, review, pinning, and specialised component validation. Any merge invalidates affected exact-head evidence and requires revalidation; it does not justify a blanket version freeze.
