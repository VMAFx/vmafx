# Architectural Decision Records (ADR)

This is the **canonical, tracked** decision log for the fork. Every non-trivial
architectural / policy / scope decision lands here as its own markdown file
before the corresponding commit merges.

## Format

We use [Michael Nygard's ADR format](https://cognitect.com/blog/2011/11/15/documenting-architecture-decisions)
(MADR-style), one markdown file per decision — not a mega-table. See
[joelparkerhenderson/architecture-decision-record](https://github.com/joelparkerhenderson/architecture-decision-record)
for background.

Each ADR file is named `NNNN-kebab-case-title.md` with a zero-padded 4-digit ID
and follows the structure in [0000-template.md](0000-template.md):

```markdown
# ADR-NNNN: <short, declarative title>

- **Status**: Proposed | Accepted | Deprecated | Superseded by [ADR-NNNN](NNNN-title.md)
- **Date**: YYYY-MM-DD
- **Deciders**: <names / handles>
- **Tags**: <comma-separated area tags>

## Context              — the problem, the forces at play
## Decision             — one paragraph in active voice
## Alternatives considered  — at minimum the runner-up, in a pros/cons table
## Consequences         — Positive / Negative / Neutral-follow-ups
## References           — upstream docs, prior ADRs, related PRs, popup-answer source
```

## Conventions

- **Filename**: `NNNN-kebab-case-title.md`. IDs are assigned in commit order
  and never reused.
- **Immutable once Accepted**: the body is frozen. To change a decision, write
  a new ADR with `Status: Supersedes ADR-NNNN` and flip the old one to
  `Superseded by ADR-MMMM`.
- **One decision per ADR** — if you find yourself writing "and also…", split it.
- **Tagging**: use the flat tag palette below so `grep -l 'Tags:.*cuda'
  docs/adr/*.md` works. New tags are fine when justified.
- **Link from per-package AGENTS.md**: the relevant per-package `AGENTS.md`
  points to the ADRs that govern that subtree, so the rationale is one click
  away from the code.
- **Backfill policy**: ADRs ≤ 0099 are *backfills* — decisions made before
  the ADR practice was formalised on 2026-04-17, captured retroactively from
  commit history and planning dossiers. Their `Status` reflects the current
  code, not the original decision date. New decisions start at 0100.

### Tag palette

`ai`, `agents`, `build`, `ci`, `claude`, `cli`, `cuda`, `dnn`, `docs`, `framework`,
`git`, `github`, `license`, `lint`, `matlab`, `mcp`, `planning`, `python`,
`readme`, `release`, `security`, `simd`, `supply-chain`, `sycl`, `testing`,
`workspace`.

## Why it exists

A Claude session makes a decision (directory move, CI gate change, dependency
swap), commits it, the session ends, and the rationale is recoverable only from
the commit message — which typically summarises the *what* but omits the
*alternatives considered*. ADRs preserve "we chose X over Y because Z" in a
single auditable place. See [ADR-0028](0028-adr-maintenance-rule.md).

## What counts as non-trivial?

Another engineer could reasonably have chosen differently. Examples:

- Directory moves (e.g., [ADR-0026](0026-workspace-relocated-under-python.md):
  `workspace/` → `python/vmaf/workspace/`)
- Base-image / dependency policy (e.g.,
  [ADR-0027](0027-non-conservative-image-pins.md): non-conservative CUDA pins)
- CI-gate semantics (e.g., [ADR-0024](0024-netflix-golden-preserved.md):
  Netflix golden tests as required status)
- Test-selection / regeneration rules
- Coding-standards changes (e.g.,
  [ADR-0012](0012-coding-standards-jpl-cert-misra.md))
- New user-visible flags or surfaces (e.g.,
  [ADR-0023](0023-tinyai-user-surfaces.md))

**Not** ADR-worthy: bug fixes, implementation details, one-off refactors that
don't change any interface or policy.

## Relation to `.workingdir2/`

Planning dossiers live under `.workingdir2/` (gitignored). Mirrored copies of
ADRs may exist there for local session continuity, but the tracked
`docs/adr/` tree is authoritative.

## Index

| ID | Title | Status | Tags |
| --- | --- | --- | --- |
| [ADR-0001](0001-stash-benchmark-noise-file.md) | Treat uncommitted benchmark result JSON as noise | Accepted | workspace, git, testing |
| [ADR-0002](0002-merge-path-master-default.md) | Merge path gpu-opt to sycl to master, master is fork default | Accepted | git, release, workspace |
| [ADR-0003](0003-workingdir2-empty-planning-dir.md) | Introduce .workingdir2 as new planning directory | Accepted | workspace, planning, claude |
| [ADR-0004](0004-auto-push-after-merges.md) | Auto-push sycl and master to origin after merges | Accepted | git, ci, release |
| [ADR-0005](0005-framework-adaptation-full-scope.md) | Adopt full framework adaptation scope a-g | Accepted | framework, ci, docs, build, mcp |
| [ADR-0006](0006-cli-precision-17g-default.md) | Set CLI precision default to %.17g with --precision flag | Superseded by [ADR-0119](0119-cli-precision-default-revert.md) | cli, testing, python |
| [ADR-0007](0007-claude-settings-fresh-rewrite.md) | Rewrite .claude/settings.json from scratch | Accepted | claude, agents |
| [ADR-0008](0008-readme-fork-rebrand.md) | Rewrite README with fork branding preserving Netflix attribution | Accepted | docs, readme, license |
| [ADR-0009](0009-mcp-server-tool-surface.md) | MCP server exposes four core tools | Accepted | mcp, python, framework |
| [ADR-0010](0010-sigstore-keyless-signing.md) | Sign release artifacts keyless via Sigstore | Accepted | security, release, supply-chain |
| [ADR-0011](0011-versioning-lusoris-suffix.md) | Version scheme v3.x.y-lusoris.N | Superseded by [ADR-1127](1127-single-semver-release-stream.md) | release, framework |
| [ADR-0012](0012-coding-standards-jpl-cert-misra.md) | Coding standards stack JPL + CERT + MISRA | Accepted | lint, docs, license |
| [ADR-0013](0013-local-dev-distro-matrix.md) | Support full local dev distro matrix | Accepted | build, docs, framework |
| [ADR-0014](0014-vscode-clangd-disable-ms-cpp.md) | VSCode uses clangd, disable MS C/C++ IntelliSense | Accepted | build, framework, lint |
| [ADR-0015](0015-ci-matrix-asan-ubsan-tsan.md) | CI matrix Linux/macOS/Windows with sanitizers | Accepted | ci, testing, security |
| [ADR-0016](0016-sycl-to-master-merge-conflict-policy.md) | Sycl to master merge conflict resolution policy | Accepted | git, workspace |
| [ADR-0017](0017-claude-skills-scope.md) | Claude skills scope includes domain scaffolding | Accepted | claude, agents, framework |
| [ADR-0018](0018-claude-hooks-scope.md) | Claude hooks scope includes safety and auto-format | Accepted | claude, agents, ci, git |
| [ADR-0019](0019-workingdir2-full-dossier.md) | .workingdir2 is the full planning dossier | Accepted | workspace, planning, docs |
| [ADR-0020](0020-tinyai-four-capabilities.md) | Tiny-AI scope covers all four capabilities | Accepted | ai, dnn, framework, cli |
| [ADR-0021](0021-training-stack-pytorch-lightning.md) | Training stack is PyTorch + Lightning with ONNX export | Accepted | ai, python, framework |
| [ADR-0022](0022-inference-runtime-onnx.md) | Inference runtime is ONNX Runtime via execution providers | Accepted | ai, dnn, cuda, sycl, build |
| [ADR-0023](0023-tinyai-user-surfaces.md) | Tiny-AI user surfaces span CLI, C API, ffmpeg, and training | Accepted | ai, dnn, cli, framework |
| [ADR-0024](0024-netflix-golden-preserved.md) | Preserve Netflix source-of-truth tests verbatim | Accepted | testing, ci, license |
| [ADR-0025](0025-copyright-handling-dual-notice.md) | Copyright handling preserves Netflix and adds Lusoris/Claude | Superseded by [ADR-0105](0105-copyright-handling-dual-notice.md) | license, docs |
| [ADR-0026](0026-workspace-relocated-under-python.md) | Relocate Python harness workspace under python/vmaf/ | Accepted | workspace, python, docs |
| [ADR-0027](0027-non-conservative-image-pins.md) | Non-conservative image pins with experimental toolchain flags | Accepted | ci, cuda, sycl, build, supply-chain |
| [ADR-0028](0028-adr-maintenance-rule.md) | Every non-trivial decision gets its own ADR file before the commit | Superseded by [ADR-0106](0106-adr-maintenance-rule.md) | docs, planning, agents |
| [ADR-0029](0029-resource-tree-relocated.md) | Relocate resource tree under python/vmaf/ | Accepted | workspace, python, docs |
| [ADR-0030](0030-matlab-sources-relocated.md) | Relocate MATLAB sources under python/vmaf/ | Accepted | workspace, matlab, python |
| [ADR-0031](0031-fork-docs-moved-under-docs.md) | Fork-added docs live under docs/ | Accepted | docs, workspace |
| [ADR-0032](0032-unittest-script-moved-to-scripts.md) | Relocate root unittest script to scripts/ | Accepted | testing, workspace |
| [ADR-0033](0033-codeql-config-moved-to-github.md) | Relocate CodeQL config to .github/ | Accepted | security, ci, github |
| [ADR-0034](0034-single-patches-directory.md) | Delete patches/ leftover, keep only ffmpeg-patches/ | Accepted | workspace, build |
| [ADR-0035](0035-claude-hooks-schema-fix.md) | Migrate .claude/settings.json hooks to current schema | Accepted | claude, agents |
| [ADR-0036](0036-tinyai-wave1-scope-expansion.md) | Tiny-AI Wave 1 scope expanded beyond D20–D23 | Superseded by [ADR-0107](0107-tinyai-wave1-scope-expansion.md) | ai, dnn, cli, framework, mcp |
| [ADR-0037](0037-master-branch-protection.md) | Protect master branch on GitHub with required checks | Accepted | github, ci, security, release |
| [ADR-0038](0038-purge-upstream-matlab-mex-binaries.md) | Purge upstream MATLAB MEX compiled binaries from tree | Accepted | security, matlab, supply-chain |
| [ADR-0039](0039-onnx-runtime-op-walk-registry.md) | Pull forward runtime op-allowlist walk and model registry | Accepted | ai, dnn, security, supply-chain |
| [ADR-0040](0040-dnn-session-multi-input-api.md) | Extend DNN session API to multi-input/multi-output with named bindings | Accepted | ai, dnn, cli |
| [ADR-0041](0041-lpips-sq-extractor.md) | Ship LPIPS-SqueezeNet FR extractor with inverse-ImageNet in graph | Accepted | ai, dnn, cli |
| [ADR-0042](0042-tinyai-docs-required-per-pr.md) | Tiny-AI PRs must ship human-readable docs in the same PR | Accepted | ai, dnn, docs |
| [ADR-0100](0100-project-wide-doc-substance-rule.md) | Every user-discoverable change ships docs in the same PR | Accepted | docs, agents, framework |
| [ADR-0101](0101-sycl-usm-picture-pool.md) | SYCL USM-backed picture pre-allocation pool | Accepted | `sycl`, `gpu`, `picture-api`, `memory` |
| [ADR-0102](0102-dnn-ep-selection-and-fp16-io.md) | DNN execution-provider selection is ordered + graceful, fp16_io does a host-side cast | Accepted | ai, dnn, api |
| [ADR-0103](0103-sycl-d3d11-surface-import.md) | `vmaf_sycl_import_d3d11_surface` ships as a staging-texture H2D path, not zero-copy | Accepted | sycl, windows, api |
| [ADR-0104](0104-picture-pool-always-on.md) | Compile `picture_pool` unconditionally and size it for the live-picture set | Accepted | api, build, cli |
| [ADR-0105](0105-copyright-handling-dual-notice.md) | Copyright handling preserves Netflix and adds Lusoris/Claude | Accepted | license, docs |
| [ADR-0106](0106-adr-maintenance-rule.md) | Every non-trivial decision gets its own ADR file before the commit | Accepted | docs, planning, agents |
| [ADR-0107](0107-tinyai-wave1-scope-expansion.md) | Tiny-AI Wave 1 scope expanded beyond ADR-0020 through ADR-0023 | Accepted | ai, dnn, cli, framework, mcp |
| [ADR-0108](0108-deep-dive-deliverables-rule.md) | Every fork-local PR ships the six deep-dive deliverables | Accepted | docs, agents, framework, planning |
| [ADR-0109](0109-nightly-bisect-model-quality.md) | Nightly bisect-model-quality runs against a synthetic placeholder cache | Accepted | ai, ci, tiny-ai, framework |
| [ADR-0110](0110-coverage-gate-fprofile-update-atomic.md) | Coverage gate `-fprofile-update=atomic` for parallel meson tests | Superseded by [ADR-0111](0111-coverage-gate-gcovr-with-ort.md) | ci, build, simd, testing |
| [ADR-0111](0111-coverage-gate-gcovr-with-ort.md) | Coverage gate `lcov` → `gcovr` with ORT in the coverage job | Accepted | ci, build, dnn, testing |
| [ADR-0112](0112-ort-backend-testability-surface.md) | Testability surface for `ort_backend.c` static helpers | Accepted | dnn, testing, coverage |
| [ADR-0113](0113-ort-create-session-fallback-multi-ep-ci.md) | ORT CreateSession fallback to CPU + multi-EP CI install | Accepted | dnn, ci, coverage, ort |
| [ADR-0114](0114-coverage-gate-per-file-overrides.md) | Per-file coverage-gate overrides for ort_backend.c + dnn_api.c | Accepted | ci, coverage, dnn, ort, gate |
| [ADR-0115](0115-ci-trigger-master-only-and-matrix-consolidation.md) | CI workflows trigger on `master` only; consolidate windows.yml into libvmaf.yml | Accepted | ci, github, build, framework |
| [ADR-0116](0116-ci-workflow-naming-convention.md) | CI workflow naming convention — purpose-named files + Title Case display names | Accepted | ci, github, docs |
| [ADR-0117](0117-coverage-gate-warning-noise-suppression.md) | Coverage-Gate annotation cleanup (gcov hits + upload-artifact) | Accepted | ci, coverage, gcovr, github-actions |
| [ADR-0118](0118-ffmpeg-patch-series-application.md) | FFmpeg patches ship as ordered series.txt, not a single carry | Accepted | ci, build, ffmpeg, docker, sycl, ai |
| [ADR-0119](0119-cli-precision-default-revert.md) | Revert CLI precision default to %.6f to honour Netflix golden gate | Accepted | cli, testing, python, golden-gate |
| [ADR-0120](0120-ai-enabled-ci-matrix-legs.md) | DNN-enabled matrix legs across compilers + macOS | Accepted | ci, ai, dnn, ort, build, github-actions |
| [ADR-0121](0121-windows-gpu-build-only-legs.md) | Windows GPU build-only matrix legs (MSVC + CUDA, MSVC + oneAPI SYCL) | Accepted | ci, build, cuda, sycl, github-actions |
| [ADR-0122](0122-cuda-gencode-coverage-and-init-hardening.md) | CUDA gencode coverage + actionable init-failure logging | Accepted | `cuda`, `build`, `docs` |
| [ADR-0123](0123-cuda-post-cubin-load-regression-32b115df.md) | CUDA prev_ref null-deref on ffmpeg libvmaf_cuda path | Accepted | `cuda`, `regression`, `upstream-sync` |
| [ADR-0124](0124-automated-rule-enforcement.md) | Automate enforcement of process ADRs (0100 / 0105 / 0106 / 0108) | Accepted | ci, agents, framework, docs, license |
| [ADR-0125](0125-ms-ssim-decimate-simd.md) | MS-SSIM decimate SIMD fast paths (AVX2 + AVX-512) | Accepted (amended 2026-04-20 — separable-form chosen with | simd, testing, agents |
| [ADR-0126](0126-ssimulacra2-extractor.md) | SSIMULACRA 2 perceptual metric as a fork-local feature extractor | Accepted | metrics, feature-extractor, docs, agents |
| [ADR-0127](0127-vulkan-compute-backend.md) | Vulkan compute backend — vendor-neutral GPU path alongside CUDA/SYCL/HIP | Accepted | gpu, vulkan, backend, build, agents |
| [ADR-0128](0128-embedded-mcp-in-libvmaf.md) | Embedded MCP server in libvmaf — SSE + UDS + stdio transports, build-flag-gated | Accepted | mcp, agents, api, build, docs |
| [ADR-0129](0129-tinyai-ptq-quantization.md) | Tiny-AI post-training int8 quantisation — static + dynamic + QAT per model | Accepted | ai, onnx, quantization, model, docs |
| [ADR-0130](0130-ssimulacra2-scalar-implementation.md) | SSIMULACRA 2 scalar implementation | Accepted | `metrics`, `feature-extractor`, `ssimulacra2` |
| [ADR-0131](0131-port-netflix-1382-cumemfree.md) | Port Netflix#1382 — `cuMemFreeAsync` → `cuMemFree` in `vmaf_cuda_picture_free` | Accepted | cuda, upstream-port, correctness |
| [ADR-0132](0132-port-netflix-1406-feature-collector-model-list.md) | Port Netflix#1406 — `feature_collector` mount/unmount model-list bugfix | Accepted | upstream-port, correctness, testing |
| [ADR-0133](0133-ci-clang-tidy-push-delta.md) | Clang-Tidy push-event should scan push delta, not full tree | Accepted | ci, lint, clang-tidy |
| [ADR-0134](0134-port-netflix-1451-meson-declare-dependency.md) | Port Netflix#1451 — `meson declare_dependency` + `override_dependency` for libvmaf | Accepted | build, upstream-port |
| [ADR-0135](0135-port-netflix-1424-expose-builtin-model-versions.md) | Port Netflix#1424 — expose built-in VMAF model-version iterator | Accepted | api, upstream-port, correctness |
| [ADR-0136](0136-ci-deliverables-checker-strip-markdown.md) | Strip markdown emphasis/code characters before ADR-0108 deliverables grep | Accepted | ci, rule-enforcement, adr-0108 |
| [ADR-0137](0137-thread-local-locale-for-numeric-io.md) | Thread-local locale handling for numeric I/O | Accepted | port, libvmaf, i18n, thread-safety, upstream-port |
| [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) | `_iqa_convolve` AVX2 bit-exact double-precision fast path | Accepted | simd, performance |
| [ADR-0139](0139-ssim-simd-bitexact-double.md) | SSIM SIMD accumulate bit-exact to scalar via per-lane scalar double | Accepted | simd, performance, bit-exact |
| [ADR-0140](0140-simd-dx-framework.md) | SIMD DX framework — header macros + scaffolding skill | Accepted | simd, dx, build, agents |
| [ADR-0141](0141-touched-file-cleanup-rule.md) | Every PR leaves its touched files lint-clean | Superseded by [ADR-1267](1267-whole-tree-zero-debt-completion.md) | ci, process, code-quality, agents |
| [ADR-0142](0142-port-netflix-18e8f1c5-vif-sigma-nsq.md) | Port Netflix upstream `vif_sigma_nsq` feature parameter | Accepted | upstream-port, feature-param, vif, simd |
| [ADR-0143](0143-port-netflix-f3a628b4-generalized-avx-convolve.md) | Port Netflix upstream generalised AVX convolve for arbitrary filter widths | Accepted | upstream-port, simd, avx2, vif, convolve |
| [ADR-0145](0145-motion-v2-neon-bitexact.md) | `motion_v2` NEON SIMD — bit-exact to scalar | Accepted | simd, neon, motion, bit-exact, performance |
| [ADR-0146](0146-nolint-sweep-function-size.md) | Sweep `readability-function-size` NOLINTs from libvmaf | Accepted | lint, cleanup, refactor, touched-file-rule |
| [ADR-0147](0147-thread-pool-job-pool.md) | Thread-pool job-object recycling + inline data buffer | Accepted | performance, threading, upstream-port |
| [ADR-0148](0148-iqa-rename-and-cleanup.md) | IQA reserved-identifier rename + touched-file lint cascade | Accepted | lint, cleanup, refactor, iqa, touched-file-rule |
| [ADR-0149](0149-port-netflix-1376-fifo-semaphore.md) | Port Netflix #1376 — FIFO-hang fix via `multiprocessing.Semaphore` | Accepted | upstream-port, python, concurrency, fifo |
| [ADR-0150](0150-port-netflix-1472-cuda-windows.md) | Port Netflix #1472 — CUDA feature extraction on Windows (MSYS2/MinGW) | Accepted | upstream-port, cuda, windows, mingw, build |
| [ADR-0151](0151-i686-ci-netflix-1481.md) | i686 build-only CI job — reproduce Netflix #1481 | Superseded by [ADR-1258](1258-keep-64-bit-only-retire-i686-lane.md) | ci, build, x86, netflix-upstream |
| [ADR-0152](0152-vmaf-read-pictures-monotonic-index.md) | `vmaf_read_pictures` rejects non-monotonic indices | Accepted | api, correctness, motion, netflix-upstream |
| [ADR-0153](0153-float-ms-ssim-min-dim-netflix-1414.md) | `float_ms_ssim` init rejects input below 176×176 | Accepted | correctness, ms-ssim, netflix-upstream |
| [ADR-0154](0154-score-pooled-eagain-netflix-755.md) | `vmaf_score_pooled` returns `-EAGAIN` for pending features | Accepted | api, correctness, motion, netflix-upstream |
| [ADR-0155](0155-adm-i4-rounding-deferred-netflix-955.md) | Defer fix for Netflix#955 — `i4_adm_cm` int32 rounding overflow | Accepted | correctness, adm, netflix-upstream, deferred, golden-gate |
| [ADR-0156](0156-cuda-graceful-error-propagation-netflix-1420.md) | CUDA backend: graceful error propagation (Netflix#1420) | Accepted | cuda, correctness, api, netflix-upstream, reliability |
| [ADR-0157](0157-cuda-preallocation-leak-netflix-1300.md) | CUDA preallocation memory leak fix + `vmaf_cuda_state_free` public API (Netflix#1300) | Accepted | cuda, correctness, api, netflix-upstream, memory |
| [ADR-0158](0158-netflix-1486-motion-updates-verified-present.md) | Netflix#1486 "Port motion updates" — verified present in fork | Accepted | upstream-port, motion, netflix-upstream, verification |
| [ADR-0159](0159-psnr-hvs-avx2-bitexact.md) | `psnr_hvs` AVX2 port — bit-exact DCT vectorization (T3-5) | Accepted | simd, avx2, psnr-hvs, bit-exact, performance |
| [ADR-0160](0160-psnr-hvs-neon-bitexact.md) | `psnr_hvs` NEON port — bit-exact DCT vectorization (T3-5-neon) | Accepted | simd, neon, aarch64, psnr-hvs, bit-exact, performance |
| [ADR-0161](0161-ssimulacra2-simd-bitexact.md) | SSIMULACRA 2 SIMD bit-exact ports — AVX2 + AVX-512 + NEON (T3-1 + T3-2) | Accepted | simd, avx2, avx512, neon, ssimulacra2, bit-exact, performance |
| [ADR-0162](0162-ssimulacra2-iir-blur-simd.md) | SSIMULACRA 2 IIR blur SIMD ports — AVX2 + AVX-512 + NEON (T3-1 phase 2) | Accepted | simd, avx2, avx512, neon, ssimulacra2, bit-exact, iir-blur, performance |
| [ADR-0163](0163-ssimulacra2-ptlr-simd.md) | SSIMULACRA 2 `picture_to_linear_rgb` SIMD ports (T3-1 phase 3) | Accepted | simd, avx2, avx512, neon, ssimulacra2, bit-exact, yuv-rgb, srgb-eotf |
| [ADR-0164](0164-ssimulacra2-snapshot-gate.md) | SSIMULACRA 2 snapshot-JSON regression gate (T3-3) | Accepted | test, ssimulacra2, regression-gate, fork-local |
| [ADR-0165](0165-state-md-bug-tracking.md) | Tracked `docs/state.md` for bug-status hygiene (T7-1) | Accepted | process, state-hygiene, claude-rule, fork-local |
| [ADR-0166](0166-mcp-server-release-channel.md) | MCP server release artifact channel — PyPI + GitHub release attachment + Sigstore (T7-2) | Accepted | release, mcp, supply-chain, sigstore, pypi |
| [ADR-0167](0167-doc-drift-enforcement.md) | Path-mapped doc-drift enforcement (local hook + CI gate) | Accepted | process, enforcement, claude-hook, ci, adr-0100 |
| [ADR-0168](0168-tinyai-konvid-baselines.md) | Tiny-AI Wave 1 baselines C2 + C3 — KoNViD-1k training (T6-1) | Accepted | tiny-ai, training, onnx, konvid-1k, c2, c3, fork-local |
| [ADR-0169](0169-onnx-allowlist-loop-if.md) | ONNX op-allowlist — admit `Loop` + `If` with recursive subgraph scan (T6-5) | Accepted | tiny-ai, onnx, security, op-allowlist |
| [ADR-0170](0170-vmaf-pre-10bit-chroma.md) | `vmaf_pre` extended to 10/12-bit and optional chroma (T6-4) | Accepted | tiny-ai, ffmpeg, dnn, api, fork-local |
| [ADR-0171](0171-bounded-loop-trip-count.md) | Bounded `Loop.M` trip-count guard (T6-5b) | Accepted | tiny-ai, onnx, security, op-allowlist |
| [ADR-0172](0172-mcp-describe-worst-frames.md) | MCP `describe_worst_frames` tool with VLM fallback (T6-6) | Accepted | tiny-ai, mcp, vlm, fork-local |
| [ADR-0173](0173-ptq-int8-audit-impl.md) | PTQ int8 audit implementation — registry schema + scripts + CI gate (T5-3) | Accepted | tiny-ai, onnx, quantization, registry, ci, fork-local |
| [ADR-0174](0174-first-model-quantisation.md) | First per-model PTQ — `learned_filter_v1` dynamic int8 (T5-3b) | Accepted | tiny-ai, onnx, quantization, registry, ci, fork-local |
| [ADR-0175](0175-vulkan-backend-scaffold.md) | Vulkan compute backend — scaffold-only audit-first PR (T5-1) | Accepted | gpu, vulkan, scaffold, audit-first, fork-local |
| [ADR-0176](0176-vulkan-vif-cross-backend-gate.md) | Vulkan VIF cross-backend gate (lavapipe + Arc nightly) | Accepted (errata 2026-04-26 below — body unchanged per ADR-0028) | `ci`, `vulkan`, `gpu`, `numerical-correctness` |
| [ADR-0177](0177-vulkan-motion-kernel.md) | Vulkan motion kernel + motion cross-backend gate | Accepted (errata 2026-04-26 below — body unchanged per ADR-0028) | `vulkan`, `gpu`, `feature-extractor`, `numerical-correctness` |
| [ADR-0178](0178-vulkan-adm-kernel.md) | Vulkan ADM kernel (T5-1c-adm) | Accepted | `vulkan`, `gpu`, `feature-extractor`, `numerical-correctness` |
| [ADR-0179](0179-float-moment-simd.md) | float_moment SIMD parity (AVX2 + NEON) | Accepted | simd, x86, arm64, feature-extractor, fork-local |
| [ADR-0180](0180-cpu-coverage-audit.md) | CPU coverage matrix audit — close 5 stale gaps | Accepted | simd, audit, fork-local, doc-correction |
| [ADR-0181](0181-feature-characteristics-registry.md) | Global feature-characteristics registry + per-backend dispatch strategy | Accepted | gpu, cuda, sycl, vulkan, architecture, fork-local |
| [ADR-0182](0182-gpu-long-tail-batch-1.md) | GPU long-tail batch 1 — psnr + ciede + moment on CUDA / SYCL / Vulkan | Accepted | gpu, cuda, sycl, vulkan, feature-extractor, fork-local |
| [ADR-0183](0183-ffmpeg-libvmaf-sycl-filter.md) | `libvmaf_sycl` FFmpeg filter — zero-copy QSV / VAAPI import | Accepted | sycl, ffmpeg, fork-local, zero-copy |
| [ADR-0184](0184-vulkan-image-import-scaffold.md) | Vulkan VkImage import C-API scaffold (T7-29 part 1 of 2) | Accepted | vulkan, ffmpeg, fork-local, zero-copy, scaffold |
| [ADR-0185](0185-vulkan-hide-volk-symbols.md) | Hide volk / Vulkan-loader symbols from libvmaf's public ABI | Accepted | vulkan, build, fork-local, abi |
| [ADR-0186](0186-vulkan-image-import-impl.md) | Vulkan VkImage import + filter (T7-29 parts 2 + 3) | Accepted | vulkan, ffmpeg, fork-local, zero-copy, implementation |
| [ADR-0187](0187-ciede-vulkan.md) | ciede2000 Vulkan kernel — float-precision per-pixel ΔE | Accepted | vulkan, gpu, feature-extractor, fork-local, places-2 |
| [ADR-0188](0188-gpu-long-tail-batch-2.md) | GPU long-tail batch 2 — psnr_hvs / ssim / ms_ssim across CUDA / SYCL / Vulkan | Accepted | gpu, cuda, sycl, vulkan, feature-extractor, fork-local |
| [ADR-0189](0189-ssim-vulkan.md) | float_ssim Vulkan kernel — host decimation, 2-dispatch GPU | Accepted | vulkan, gpu, feature-extractor, fork-local, places-4 |
| [ADR-0190](0190-ms-ssim-vulkan.md) | float_ms_ssim Vulkan kernel — 5-level pyramid + Wang product on host | Accepted | vulkan, gpu, feature-extractor, fork-local, places-4 |
| [ADR-0191](0191-psnr-hvs-vulkan.md) | float_psnr_hvs Vulkan kernel — overlapping 8×8 DCT blocks + per-plane log transform | Accepted | vulkan, gpu, feature-extractor, fork-local, places-2 |
| [ADR-0192](0192-gpu-long-tail-batch-3.md) | GPU long-tail batch 3 — closing every remaining metric gap (motion_v2 / float_ansnr / ssimulacra2 / cambi + float twins) | Accepted | gpu, cuda, sycl, vulkan, feature-extractor, fork-local |
| [ADR-0193](0193-motion-v2-vulkan.md) | motion_v2 Vulkan kernel — single-dispatch SAD via convolution linearity | Accepted | vulkan, gpu, feature-extractor, fork-local, bit-exact |
| [ADR-0194](0194-float-ansnr-gpu.md) | float_ansnr GPU kernels — single-dispatch 3x3 + 5x5 filters with per-WG float partials | Accepted | vulkan, cuda, sycl, gpu, feature-extractor, fork-local, places-4 |
| [ADR-0195](0195-float-psnr-gpu.md) | float_psnr GPU kernels — single-dispatch diff² with float partials, bit-exact vs CPU | Accepted | vulkan, cuda, sycl, gpu, feature-extractor, fork-local, bit-exact |
| [ADR-0196](0196-float-motion-gpu.md) | float_motion GPU kernels — float twin of integer_motion blur+SAD | Accepted | vulkan, cuda, sycl, gpu, feature-extractor, fork-local, places-4 |
| [ADR-0197](0197-float-vif-gpu.md) | float_vif GPU kernels — 4-scale pyramid with mirror-asymmetry fix | Accepted | vulkan, cuda, sycl, gpu, feature-extractor, fork-local, places-4 |
| [ADR-0198](0198-volk-priv-remap-static-archive.md) | Rename volk's `vk*` symbols to `vmaf_priv_vk*` for static-archive builds | Accepted | vulkan, build, fork-local, abi |
| [ADR-0199](0199-float-adm-vulkan.md) | float_adm Vulkan kernel — sixth Group B float twin | Accepted | vulkan, gpu, feature-extractor, fork-local, places-4 |
| [ADR-0200](0200-volk-priv-remap-pkgconfig-leak-fix.md) | Move volk `-include` flag off of `volk_dep.compile_args` (libvmaf.pc leak fix) | Accepted | vulkan, build, fork-local, abi |
| [ADR-0201](0201-ssimulacra2-vulkan-kernel.md) | ssimulacra2 Vulkan kernel | Accepted | vulkan, gpu, ssimulacra2, precision |
| [ADR-0202](0202-float-adm-cuda-sycl.md) | float_adm CUDA + SYCL twins — sixth Group B float kernel finishes | Accepted | cuda, sycl, gpu, feature-extractor, fork-local, places-4 |
| [ADR-0203](0203-tiny-ai-training-prep-impl.md) | Tiny-AI training prep — implementation decisions | Accepted | `ai`, `training`, `fork-local`, `onnx`, `docs` |
| [ADR-0205](0205-cambi-gpu-feasibility.md) | cambi GPU feasibility spike | Accepted | vulkan, gpu, cambi, feasibility-spike, fork-local |
| [ADR-0206](0206-ssimulacra2-cuda-sycl.md) | ssimulacra2 CUDA + SYCL twins | Accepted | cuda, sycl, gpu, ssimulacra2, precision |
| [ADR-0207](0207-tinyai-qat-design.md) | Tiny-AI Quantization-Aware Training (QAT) — design | Accepted | ai, quantization, dnn, tiny-ai, fork-local |
| [ADR-0208](0208-learned-filter-v1-qat-impl.md) | First per-model QAT — `learned_filter_v1` int8 (T5-4) | Accepted | tiny-ai, onnx, quantization, qat, registry, ci, fork-local |
| [ADR-0209](0209-mcp-embedded-scaffold.md) | Embedded MCP server — scaffold-only audit-first PR (T5-2) | Accepted | mcp, agents, api, scaffold, audit-first, fork-local |
| [ADR-0210](0210-cambi-vulkan-integration.md) | cambi Vulkan integration (Strategy II hybrid) | Accepted | vulkan, gpu, cambi, feature-extractor, fork-local, places-4 |
| [ADR-0211](0211-model-registry-sigstore.md) | Tiny-model registry schema + Sigstore `--tiny-model-verify` | Accepted | ai, dnn, security, supply-chain, fork-local, t6-9 |
| [ADR-0212](0212-hip-backend-scaffold.md) | HIP (AMD ROCm) compute backend — scaffold-only audit-first PR (T7-10) | Accepted | gpu, hip, rocm, amd, scaffold, audit-first, fork-local |
| [ADR-0213](0213-ssimulacra2-sve2.md) | SSIMULACRA 2 SVE2 SIMD parity | Accepted | simd, arm64, sve2, ssimulacra2, qemu, ci |
| [ADR-0214](0214-gpu-parity-ci-gate.md) | GPU-parity CI gate (T6-8) — cross-device variance matrix | Accepted | ci, gpu, vulkan, cuda, sycl, agents, fork-local |
| [ADR-0215](0215-fastdvdnet-pre-filter.md) | FastDVDnet temporal pre-filter — 5-frame window, placeholder weights | Accepted | `ai`, `dnn`, `feature-extractor`, `wave-1` |
| [ADR-0216](0216-vulkan-chroma-psnr.md) | Vulkan PSNR — chroma extension (psnr_cb / psnr_cr) | Accepted | `vulkan`, `gpu`, `feature-extractor`, `psnr` |
| [ADR-0217](0217-sycl-toolchain-cleanup.md) | SYCL toolchain cleanup — multi-version recipe + icpx-aware clang-tidy wrapper | Accepted | sycl, ci, clang-tidy, tooling, fork-local |
| [ADR-0218](0218-mobilesal-saliency-extractor.md) | MobileSal saliency feature extractor (T6-2a) | Accepted | ai, dnn, feature-extractor, saliency, fork-local |
| [ADR-0219](0219-motion3-gpu-coverage.md) | motion3 GPU coverage on Vulkan + CUDA + SYCL (3-frame window) | Accepted | gpu, vulkan, cuda, sycl, motion, feature-extractor, fork-local, t3-15c, places-4 |
| [ADR-0220](0220-sycl-fp64-fallback.md) | SYCL feature kernels are unconditionally fp64-free | Accepted | sycl, perf, gpu, arc, intel, t7-17 |
| [ADR-0221](0221-changelog-adr-fragment-pattern.md) | CHANGELOG + ADR-index fragment-file pattern | Accepted | process, release, docs, ci, fork-local |
| [ADR-0222](0222-vmaf-per-shot-tool.md) | `vmaf-perShot` per-shot CRF predictor sidecar | Accepted | ai, tools, encoder-hint, fork-local, t6-3b |
| [ADR-0223](0223-transnet-v2-shot-detector.md) | TransNet V2 shot-boundary detector — 100-frame window, placeholder weights | Accepted | `ai`, `dnn`, `feature-extractor`, `wave-1`, `shot-detection`, `fork-local` |
| [ADR-0234](0234-gpu-gen-ulp-calibration.md) | GPU-generation-aware ULP calibration head | Accepted (2026-05-03 — calibration-table tier landed; ONNX-head | ai, gpu, vulkan, cuda, sycl, cross-backend, fork-local, t7-gpu-ulp-cal |
| [ADR-0235](0235-codec-aware-fr-regressor.md) | Codec-aware FR regressor (`fr_regressor_v2`) | Accepted | `ai`, `dnn`, `tiny-ai`, `fr-regressor`, `fork-local` |
| [ADR-0236](0236-dists-extractor.md) | DISTS extractor as LPIPS companion | Accepted | ai, fr, dnn, tiny-ai, fork-local, perceptual |
| [ADR-0237](0237-quality-aware-encode-automation.md) | Quality-aware encode automation surface (`vmaf-tune`) | Accepted (Phase A only; Phases B–F remain Proposed) | tooling, ai, ffmpeg, codec, automation, fork-local |
| [ADR-0238](0238-vulkan-picture-preallocation.md) | Vulkan VmafPicture preallocation surface (API parity with CUDA / SYCL) | Accepted | vulkan, api, preallocation, fork-local, parity |
| [ADR-0239](0239-gpu-picture-pool-dedup.md) | Backend-agnostic GPU picture pool (`gpu_picture_pool.{h,c}`) | Accepted | refactor, gpu, cuda, sycl, vulkan, dedup, fork-local |
| [ADR-0240](0240-gpu-backend-pattern-doc.md) | GPU backend public-header pattern doc (PR3 of GPU dedup, doc-only) | Accepted | docs, gpu, agents, fork-local |
| [ADR-0241](0241-hip-first-consumer-psnr.md) | HIP first-consumer kernel — `integer_psnr_hip` via mirrored kernel-template | Accepted | gpu, hip, rocm, amd, kernel-template, fork-local |
| [ADR-0242](0242-tiny-ai-netflix-training-corpus.md) | Tiny-AI training on the original Netflix VMAF training corpus | Accepted | `ai`, `training`, `fork-local`, `onnx`, `docs` |
| [ADR-0243](0243-enable-lcs-gpu.md) | `enable_lcs` MS-SSIM extras on CUDA + Vulkan | Accepted | cuda, vulkan, gpu, metrics, ms-ssim, fork-local |
| [ADR-0244](0244-vmaf-tiny-v2.md) | vmaf_tiny_v2 — canonical-6 + StandardScaler tiny VMAF MLP | Accepted | ai, dnn, tiny-ai, model, registry, fork-local |
| [ADR-0245](0245-simd-bitexact-test-harness.md) | SIMD bit-exact test harness shared header | Accepted | simd, test, dx, fork-local |
| [ADR-0246](0246-gpu-kernel-template.md) | Per-backend GPU kernel scaffolding templates (CUDA + Vulkan) | Accepted | gpu, cuda, vulkan, refactor, fork-local |
| [ADR-0247](0247-vmaf-roi-tool.md) | vmaf-roi sidecar binary for per-CTU QP offsets | Accepted | `tools`, `ai`, `roi`, `encoder` |
| [ADR-0248](0248-nr-metric-v1-ptq.md) | `nr_metric_v1` joins dynamic-PTQ family (T5-3d) | Accepted | tiny-ai, onnx, quantization, registry, fork-local |
| [ADR-0249](0249-fr-regressor-v1.md) | Tiny-AI Wave 1 baseline C1 — `fr_regressor_v1` on Netflix Public | Accepted | tiny-ai, training, onnx, netflix-public, c1, fork-local |
| [ADR-0250](0250-tiny-ai-extractor-template.md) | Tiny-AI extractor template — shared scaffolding header | Accepted | `ai`, `dnn`, `refactor`, `dx`, `fork-local` |
| [ADR-0251](0251-vulkan-async-pending-fence.md) | Vulkan VkImage import — v2 async pending-fence model (T7-29 part 4) | Accepted | vulkan, ffmpeg, fork-local, zero-copy, performance, implementation |
| [ADR-0252](0252-ssimulacra2-host-xyb-simd.md) | ssimulacra2 Vulkan host-path AVX2 + NEON SIMD (T-GPU-OPT-VK-2) | Accepted | `simd`, `vulkan`, `ssimulacra2`, `performance` |
| [ADR-0253](0253-speed-qa-extractor.md) | Defer SpEED-QA full-reference reduction | Accepted | `metrics`, `research`, `feature-extractor`, `roadmap` |
| [ADR-0254](0254-hip-second-consumer-float-psnr.md) | HIP second-consumer kernel — `float_psnr_hip` via mirrored kernel-template | Accepted | gpu, hip, rocm, amd, kernel-template, fork-local |
| [ADR-0255](0255-fastdvdnet-pre-real-weights.md) | FastDVDnet temporal pre-filter — real upstream weights via luma adapter (T6-7b) | Accepted | `ai`, `dnn`, `feature-extractor`, `wave-1`, `weights-drop` |
| [ADR-0256](0256-vulkan-submit-opt-batch.md) | Vulkan submit-side template + fence pool + descriptor pre-alloc | Accepted | vulkan, perf, kernel-template |
| [ADR-0257](0257-mobilesal-real-weights-deferred.md) | MobileSal real-weights swap deferred (T6-2a-followup blocker) | Accepted | ai, dnn, mobilesal, saliency, license, fork-local, docs |
| [ADR-0258](0258-onnx-allowlist-resize.md) | ONNX op-allowlist — admit `Resize` for saliency / segmentation models (T7-32) | Accepted | tiny-ai, onnx, security, op-allowlist |
| [ADR-0259](0259-hip-third-consumer-ciede.md) | HIP third-consumer kernel — `ciede_hip` via mirrored kernel-template | Accepted | gpu, hip, rocm, amd, kernel-template, fork-local |
| [ADR-0260](0260-hip-fourth-consumer-float-moment.md) | HIP fourth-consumer kernel — `float_moment_hip` via mirrored kernel-template | Accepted | gpu, hip, rocm, amd, kernel-template, fork-local |
| [ADR-0261](0261-transnet-v2-real-weights.md) | TransNet V2 shot-boundary detector — real upstream weights via NTCHW adapter (T6-3a-followup) | Accepted | `ai`, `dnn`, `feature-extractor`, `wave-1`, `weights-drop`, `shot-detection`, `fork-local` |
| [ADR-0262](0262-bisect-cache-logical-comparison.md) | bisect-model-quality cache check uses logical comparison for parquet | Accepted | ai, ci, tiny-ai, framework |
| [ADR-0263](0263-ossf-scorecard-policy.md) | OSSF Scorecard policy and remediation cadence | Superseded by [ADR-1247](1247-scorecard-exact-head-gates.md) | ci, security, supply-chain, docs |
| [ADR-0264](0264-vulkan-1-4-bump-blocked-on-fp-contraction.md) | Vulkan 1.4 API-version bump blocked on shader FP-contraction audit | Accepted | vulkan, fork-local, bit-exactness, backlog, docs |
| [ADR-0265](0265-u2netp-saliency-replacement-blocked.md) | U-2-Net `u2netp` saliency replacement blocked on weights distribution + op allowlist | Accepted | ai, dnn, mobilesal, u2netp, saliency, license, op-allowlist, fork-local, docs |
| [ADR-0266](0266-hip-fifth-consumer-float-ansnr.md) | HIP fifth kernel-template consumer — `float_ansnr_hip` | Accepted | `hip`, `gpu`, `feature-extractor`, `kernel-template` |
| [ADR-0267](0267-hip-sixth-consumer-motion-v2.md) | HIP sixth kernel-template consumer — `motion_v2_hip` | Accepted | `hip`, `gpu`, `feature-extractor`, `kernel-template`, `temporal` |
| [ADR-0269](0269-vif-ciede-precise-step-a.md) | `precise` decoration audit on `vif.comp` + `ciede.comp` — Step A of the Vulkan 1.4 bump path | Accepted | vulkan, fork-local, bit-exactness, shaders |
| [ADR-0270](0270-fuzzing-scaffold.md) | libFuzzer scaffold for parser surfaces (OSSF Scorecard remediation) | Accepted | security, build, ci, docs |
| [ADR-0271](0271-cuda-drain-batch-ms-ssim.md) | Wire `integer_ms_ssim_cuda` through the CUDA fence-batching helper | Accepted | cuda, gpu, perf, fork-local |
| [ADR-0272](0272-fr-regressor-v2-codec-aware-scaffold.md) | `fr_regressor_v2` codec-aware scaffold (Phase B prereq) | Accepted | `ai`, `dnn`, `tiny-ai`, `fr-regressor`, `codec-aware`, |
| [ADR-0273](0273-hip-seventh-consumer-float-motion.md) | HIP seventh kernel-template consumer — `float_motion_hip` | Accepted | `hip`, `gpu`, `feature-extractor`, `kernel-template`, `temporal`, `fork-local` |
| [ADR-0274](0274-hip-eighth-consumer-float-ssim.md) | HIP eighth kernel-template consumer — `float_ssim_hip` | Accepted | `hip`, `gpu`, `feature-extractor`, `kernel-template`, `multi-dispatch`, `fork-local` |
| [ADR-0275](0275-vmaf-tiny-v3-v4-ptq.md) | `vmaf_tiny_v3` and `vmaf_tiny_v4` join dynamic-PTQ family (T5-3d follow-up) | Accepted | tiny-ai, onnx, quantization, registry, fork-local |
| [ADR-0276](0276-vmaf-tune-fast-path.md) | `vmaf-tune fast` — proxy-based recommend (Phase A.5) | Accepted | tooling, ai, ffmpeg, codec, automation, fork-local |
| [ADR-0277](0277-ffmpeg-patches-refresh-2026-05-04.md) | ffmpeg-patches refresh against n8.1 — 2026-05-04 (no drift) | Accepted | ffmpeg, fork-local, maintenance, patches |
| [ADR-0278](0278-t7-5-nolint-sweep.md) | T7-5 NOLINT-sweep closeout — citation normalisation across libvmaf | Accepted | lint, cleanup, touched-file-rule, t7-5 |
| [ADR-0279](0279-vmaf-tune-codec-adapter-libaom.md) | vmaf-tune codec adapter — libaom-av1 | Accepted | tools, vmaf-tune, av1, codec-adapter |
| [ADR-0281](0281-vmaf-tune-qsv-adapters.md) | `vmaf-tune` Intel QSV codec adapters (`h264_qsv`, `hevc_qsv`, `av1_qsv`) | Accepted | tooling, ffmpeg, codec, qsv, intel, fork-local |
| [ADR-0282](0282-vmaf-tune-amf-adapters.md) | `vmaf-tune` AMD AMF codec adapters (h264 / hevc / av1) | Accepted | tooling, ffmpeg, codec, amd, amf, vmaf-tune, fork-local |
| [ADR-0283](0283-vmaf-tune-videotoolbox-adapters.md) | `vmaf-tune` Apple VideoToolbox codec adapters | Accepted | tooling, ai, ffmpeg, codec, hardware-encoder, apple, fork-local |
| [ADR-0285](0285-vmaf-tune-vvenc-nnvc.md) | `vmaf-tune` libvvenc adapter — VVC / H.266 with optional NN-VC tools | Accepted | tooling, codec, vvc, h266, ai, nnvc, fork-local |
| [ADR-0286](0286-saliency-student-fork-trained-on-duts.md) | Fork-trained saliency student `saliency_student_v1` on DUTS-TR | Accepted | ai, dnn, mobilesal, saliency, training, license, fork-local, docs |
| [ADR-0287](0287-vmaf-tiny-v5-corpus-expansion.md) | vmaf_tiny_v5 — corpus expansion (4-corpus + YouTube UGC vp9 subset) | Accepted (decision: defer — no `vmaf_tiny_v5.onnx` shipped) | ai, dnn, tiny-ai, training-data, research, fork-local |
| [ADR-0288](0288-vmaf-tune-codec-adapter-x265.md) | `vmaf-tune` libx265 codec adapter | Accepted | tooling, ffmpeg, codec, automation, fork-local, vmaf-tune |
| [ADR-0289](0289-vmaf-tune-resolution-aware.md) | `vmaf-tune` resolution-aware model selection + CRF offsets | Accepted | tooling, vmaf-tune, model-selection, fork-local |
| [ADR-0290](0290-vmaf-tune-nvenc-adapters.md) | NVENC codec adapters for `vmaf-tune` (h264 / hevc / av1) | Accepted | tooling, codec, nvenc, gpu, fork-local |
| [ADR-0291](0291-fr-regressor-v2-prod-ship.md) | fr_regressor_v2 — flip from smoke to production | Accepted | ai, dnn, tiny-ai, fr-regressor, codec-aware, vmaf-tune, fork-local |
| [ADR-0293](0293-vmaf-tune-saliency-aware.md) | `vmaf-tune` saliency-aware ROI tuning (Bucket #2) | Accepted | tooling, ai, saliency, ffmpeg, codec, fork-local |
| [ADR-0294](0294-vmaf-tune-codec-adapter-svtav1.md) | vmaf-tune codec adapter for SVT-AV1 | Accepted | `tools`, `vmaf-tune`, `codec`, `av1`, `fork-local` |
| [ADR-0295](0295-vmaf-tune-phase-e-bitrate-ladder.md) | vmaf-tune Phase E — per-title bitrate-ladder generator | Accepted | tooling, ffmpeg, codec, automation, abr, fork-local |
| [ADR-0296](0296-vmaf-roi-saliency-weighted.md) | Region-of-interest VMAF scoring (`vmaf-roi-score`) — saliency-weighted scaffold | Accepted (Option C scaffold only; Option A remains Proposed) | tooling, ai, saliency, vmaf, fork-local |
| [ADR-0297](0297-vmaf-tune-encode-multi-codec.md) | `vmaf-tune` — codec-agnostic encode dispatcher | Accepted | tooling, ffmpeg, codec, automation, fork-local |
| [ADR-0298](0298-vmaf-tune-cache.md) | vmaf-tune content-addressed encode/score cache | Accepted | `tools`, `vmaf-tune`, `cache`, `fork-local` |
| [ADR-0299](0299-vmaf-tune-gpu-score.md) | GPU scoring backend for `vmaf-tune` (`--score-backend`) | Accepted | tooling, cuda, vulkan, sycl, ai, automation, fork-local |
| [ADR-0300](0300-vmaf-tune-hdr-aware.md) | `vmaf-tune` HDR-aware encoding + scoring | Accepted (encode-side flags); HDR-VMAF scoring deferred (no fork-local model JSON yet) | tooling, vmaf-tune, hdr, codec, ffmpeg, fork-local |
| [ADR-0301](0301-vmaf-tune-sample-clip.md) | `vmaf-tune --sample-clip-seconds` (sample-clip mode) | Accepted | tooling, ffmpeg, vmaf-tune, fork-local |
| [ADR-0302](0302-encoder-vocab-v3-schema-expansion.md) | ENCODER_VOCAB v3 — 16-slot schema expansion + retrain plan | Accepted | not recorded |
| [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md) | `fr_regressor_v2` ensemble — production flip trainer + CI gate | Accepted | ai, fr-regressor, ensemble, probabilistic, loso, ci-gate, fork-local |
| [ADR-0304](0304-vmaf-tune-fast-path-prod-wiring.md) | `vmaf-tune fast` — production wiring (Optuna TPE + v2 proxy + GPU verify) | Accepted | tooling, ai, ffmpeg, codec, automation, fork-local |
| [ADR-0305](0305-encoder-knob-space-pareto-analysis.md) | Encoder knob-space Pareto-frontier analysis stratified per (source, codec, rc_mode) | Accepted | ai, vmaf-tune, research, encoder, pareto, fork-local |
| [ADR-0306](0306-vmaf-tune-coarse-to-fine.md) | `vmaf-tune` coarse-to-fine CRF search | Accepted | tooling, automation, vmaf-tune, ffmpeg |
| [ADR-0307](0307-vmaf-tune-ladder-default-sampler.md) | `vmaf-tune` ladder default sampler — wire Phase B/E gap | Accepted | tooling, automation, vmaf-tune, ladder, fork-local |
| [ADR-0308](0308-encoder-knob-sweep-recipe-regression-policy.md) | Encoder knob-sweep recipe-regression revision policy | Accepted | ai, vmaf-tune, codec-adapters, knob-sweep, fork-local |
| [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md) | `fr_regressor_v2` ensemble — real-corpus retrain harness + flip workflow | Accepted | ai, fr-regressor, ensemble, loso, runbook, fork-local |
| [ADR-0310](0310-bvi-dvc-corpus-ingestion.md) | BVI-DVC corpus ingestion for `fr_regressor_v2` | Accepted | ai, training, corpus, license, fork-local |
| [ADR-0311](0311-libfuzzer-harness-expansion.md) | libFuzzer harness expansion — `fuzz_yuv_input` + `fuzz_cli_parse` | Accepted | security, build, ci, docs, fork-local |
| [ADR-0312](0312-ffmpeg-patches-vmaf-tune-integration.md) | FFmpeg-patch series for vmaf-tune integration (qpfile + libvmaf_tune + pass-autotune) | Accepted | tooling, ffmpeg, vmaf-tune, patch-series, scaffold |
| [ADR-0313](0313-ci-required-checks-aggregator.md) | CI required-checks aggregator (unblock doc/Python-only PRs) | Accepted | ci, branch-protection, policy, fork-local |
| [ADR-0314](0314-vmaf-tune-score-backend-vulkan.md) | vmaf-tune `--score-backend=vulkan` (vendor-neutral GPU scoring) | Accepted | tooling, vmaf-tune, vulkan, gpu, fork-local |
| [ADR-0315](0315-vendor-neutral-vvc-encode-strategy.md) | Vendor-neutral VVC encode strategy — tiered Tier-1-now / Tier-2-backlog / Tier-3-revisit | Accepted | codecs, vvc, h266, gpu, hip, sycl, vulkan-video, vmaf-tune, nn-vc, fork-local |
| [ADR-0316](0316-cli-parse-long-only-error-fix.md) | cli_parse — handle long-only options in `error()` | Accepted | cli, security, fork-local, fuzzing |
| [ADR-0317](0317-ci-doc-only-pr-flake-fix.md) | Path-filter Docker + FFmpeg-integration on doc/Python-only PRs | Accepted | `ci`, `build` |
| [ADR-0318](0318-ensemble-retrain-harness-fix.md) | `fr_regressor_v2` ensemble retrain harness — wrapper-trainer interface fix + Phase A pre-step doc | Accepted | `ai`, `fr-regressor`, `ensemble`, `loso`, `runbook`, `fork-local` |
| [ADR-0319](0319-ensemble-loso-trainer-real-impl.md) | `fr_regressor_v2` ensemble LOSO trainer — real loader + per-fold training | Accepted | ai, fr-regressor, ensemble, loso, fork-local |
| [ADR-0320](0320-fr-regressor-v2-ensemble-seed-flip.md) | `fr_regressor_v2` ensemble seeds — production flip (smoke → false) | Accepted | ai, fr-regressor, ensemble, registry, prod-flip, fork-local |
| [ADR-0321](0321-fr-regressor-v2-ensemble-full-prod-flip.md) | `fr_regressor_v2_ensemble_v1` — full production flip (real ONNX + sidecars) | Accepted | `ai`, `tinyai`, `models`, `registry`, `prod-flip` |
| [ADR-0323](0323-fr-regressor-v3-train-and-register.md) | `fr_regressor_v3` — train + register on ENCODER_VOCAB v3 (16-slot) | Accepted | ai, fr-regressor, codec-aware, encoder-vocab, loso, fork-local |
| [ADR-0324](0324-ensemble-training-kit.md) | Ensemble training kit — portable Phase-A + LOSO retrain bundle | Accepted | ai, fr-regressor, ensemble, tooling, fork-local |
| [ADR-0325](0325-konvid-150k-corpus-ingestion.md) | KonViD-150k corpus ingestion | Accepted (2026-05-15 — corpus materialized at | ai, training, corpus, license, fork-local |
| [ADR-0326](0326-vmaf-tune-phase-b-bisect.md) | vmaf-tune Phase B — target-VMAF bisect | Accepted | tooling, vmaf-tune, fork-local |
| [ADR-0328](0328-cambi-cluster-port-skip-shared-header-rename.md) | Cambi cluster port — skip the shared-header rename | Accepted | simd, port, cambi |
| [ADR-0331](0331-skip-ci-on-draft-prs.md) | Skip CI on draft pull requests | Accepted | ci, build, fork-local |
| [ADR-0332](0332-agent-worktree-drift-hard-guard.md) | Agent worktree-drift hard guard | Accepted | agents, ci, build, fork-local |
| [ADR-0333](0333-vmaf-tune-multi-pass-encoding.md) | `vmaf-tune` Phase F — multi-pass encoding (libx265 first) | Accepted | tooling, ffmpeg, codec, automation, fork-local, vmaf-tune, phase-f |
| [ADR-0334](0334-state-md-touch-check-ci-gate.md) | state.md-touch-check CI gate (ADR-0165 enforcement) | Accepted | ci, process, state-hygiene, claude-rule, fork-local |
| [ADR-0335](0335-hardware-capability-priors.md) | Hardware-capability priors for the FR-regressor corpus | Accepted | `ai`, `corpus`, `data`, `docs` |
| [ADR-0336](0336-konvid-mos-head-v1.md) | KonViD MOS head v1 (ADR-0325 Phase 3) | Accepted | ai, training, mos, konvid, fork-local |
| [ADR-0337](0337-motion-v2-public-api-options.md) | motion_v2 inherits motion v1's public option surface (duplicate registration) | Accepted | upstream-port, motion, feature-extractor, cli, public-api, fork-local |
| [ADR-0338](0338-macos-vulkan-via-moltenvk-lane.md) | macOS Vulkan-via-MoltenVK CI lane (advisory) for the Vulkan backend | Accepted | ci, vulkan, macos, moltenvk, gpu, advisory |
| [ADR-0339](0339-av1-videotoolbox-placeholder-adapter.md) | `av1_videotoolbox` placeholder adapter + upstream watcher | Accepted | tooling, ai, ffmpeg, codec, hardware-encoder, apple, fork-local, upstream-blocked |
| [ADR-0340](0340-multi-corpus-aggregation.md) | Multi-corpus aggregation for the FR-regressor / predictor v2 trainer | Accepted | ai, training, corpus, fork-local |
| [ADR-0341](0341-ci-paths-ignore-doc-only-prs.md) | `paths-ignore` filter on heavy CI workflows for doc-only PRs | Accepted | `ci`, `build`, `policy`, `fork-local` |
| [ADR-0345](0345-cambi-gpu-port-strategy.md) | cambi × {CUDA, SYCL, HIP} GPU port strategy | Accepted | cuda, sycl, hip, gpu, cambi, fork-local, places-4 |
| [ADR-0346](0346-fr-features-from-nr-corpus.md) | FR-features-from-NR-corpus adapter pattern | Accepted | ai, training, corpus, methodology, fork-local |
| [ADR-0347](0347-sanitizer-matrix-test-scope.md) | Sanitizer matrix — concrete test-set scope per sanitizer | Accepted | ci, testing, sanitizer, asan, ubsan, tsan, fork-local |
| [ADR-0348](0348-codeql-poorly-documented-function-suppressed.md) | Globally suppress CodeQL `cpp/poorly-documented-function` | Accepted | `ci`, `security`, `codeql`, `policy`, `fork-local` |
| [ADR-0349](0349-fr-regressor-v3-namespace.md) | `fr_regressor_v3` namespace — reserve `_v3plus_features` for the next feature-set bump | Accepted | `ai`, `docs`, `naming` |
| [ADR-0350](0350-psnr-hvs-avx512-ceiling.md) | `psnr_hvs` AVX-512 — re-bench confirms AVX2 ceiling (T3-9 (a)) | Accepted | simd, avx512, psnr-hvs, ceiling, audit, fork-local |
| [ADR-0351](0351-cuda-chroma-psnr.md) | CUDA PSNR — chroma extension (psnr_cb / psnr_cr) | Accepted | `cuda`, `gpu`, `feature-extractor`, `psnr` |
| [ADR-0352](0352-vulkan-submit-pool-pr-a-adm-motion-psnr.md) | Vulkan submit-pool migration — PR A (adm, motion, psnr) | Accepted | vulkan, perf, kernel-template |
| [ADR-0353](0353-vulkan-submit-pool-pr-b-six-kernels.md) | Vulkan submit-pool migration PR-B — six secondary kernels | Accepted | vulkan, gpu, performance, fork-local |
| [ADR-0354](0354-vulkan-submit-pool-pr-c-four-kernels.md) | Vulkan submit-pool migration PR-C — cambi, ssimulacra2, float_ansnr, moment | Accepted | vulkan, perf, kernel-template, fork-local |
| [ADR-0355](0355-symphony-agent-dispatch-infra.md) | Symphony-inspired agent-dispatch infrastructure | Accepted | `agents`, `ci`, `tooling`, `fork-local` |
| [ADR-0356](0356-vulkan-two-level-gpu-reduction.md) | Two-level GPU reduction for Vulkan VIF / ADM / motion accumulators | Accepted | `vulkan`, `perf`, `gpu` |
| [ADR-0357](0357-vulkan-readback-alloc-flag.md) | Vulkan readback buffer VMA allocation flag separation | Accepted | vulkan, performance |
| [ADR-0358](0358-cuda-motion-race-and-precision-fixes.md) | CUDA `motion` correctness — SAD race, pinned-mem leak, and motion2/motion3 precision parity with CPU | Accepted | `cuda`, `motion`, `correctness`, `precision` |
| [ADR-0359](0359-arc-runners-pilot.md) | ARC self-hosted runner pool: pilot via `ARC_RUNNERS_ENABLED` flag | Accepted | ci, infra, fork-local |
| [ADR-0360](0360-cambi-cuda.md) | CAMBI CUDA port (Strategy II hybrid, T3-15a) | Accepted | cuda, gpu, cambi, feature-extractor, fork-local, places-4, t3-15 |
| [ADR-0361](0361-metal-compute-backend.md) | Metal compute backend — scaffold-only audit-first PR (T8-1) | Accepted | gpu, metal, apple-silicon, scaffold, audit-first, fork-local |
| [ADR-0362](0362-k150k-corpus-integration.md) | K150K-A corpus integration: FR-from-NR extraction of FULL_FEATURES | Accepted | ai, training-data, corpus, k150k, full-features, fork-local |
| [ADR-0363](0363-renovate-replaces-dependabot.md) | Mend Renovate replaces Dependabot as the dependency-update bot | Accepted | ci, security, dependencies, github-actions, pre-commit, fork-local |
| [ADR-0364](0364-saliency-student-v2-resize-decoder.md) | `saliency_student_v2` — Resize-decoder ablation on the v1 recipe | Accepted (gate passed: v2 IoU 0.7105 ≥ v1 0.6558) | ai, dnn, mobilesal, saliency, training, fork-local, docs |
| [ADR-0365](0365-coreml-ep-wiring.md) | Wire the CoreML execution provider into tiny-AI ORT dispatch | Accepted | ai, dnn, coreml, apple-silicon, fork-local |
| [ADR-0366](0366-corpus-schema-v3.md) | vmaf-tune corpus schema v3 — canonical-6 per-feature aggregates | Accepted | `ai`, `tools`, `vmaf-tune`, `corpus`, `schema` |
| [ADR-0367](0367-lsvq-corpus-ingestion.md) | LSVQ corpus ingestion for `nr_metric_v1` | Accepted | ai, training, corpus, license, fork-local |
| [ADR-0368](0368-external-bench-wrapper-only.md) | External-competitor benchmark harness — wrapper-only architecture | Accepted | ai, testing, license, tooling, fork-local |
| [ADR-0369](0369-waterloo-ivc-4k-corpus-ingestion.md) | Waterloo IVC 4K-VQA corpus ingestion for `nr_metric_v1` | Accepted | ai, training, corpus, license, fork-local |
| [ADR-0370](0370-live-vqc-corpus-ingestion.md) | LIVE-VQC MOS-corpus ingestion for `nr_metric_v1` | Accepted | ai, training, corpus, license, fork-local |
| [ADR-0371](0371-corpus-ingest-base-class.md) | Shared `CorpusIngestBase` for MOS-corpus ingestion adapters | Accepted | ai, corpus, refactor, fork-local |
| [ADR-0372](0372-hip-batch1-integer-psnr-float-ansnr.md) | HIP Batch-1 — `integer_psnr_hip` and `float_ansnr_hip` Real Kernels | Accepted | `hip`, `gpu`, `build` |
| [ADR-0373](0373-hip-batch2-float-motion.md) | HIP Batch-2 — `float_motion_hip` Real Kernel | Accepted | `hip`, `gpu`, `build` |
| [ADR-0374](0374-disabled-build-enosys-contract.md) | Build-time-optional public APIs return `-ENOSYS` when disabled | Accepted | `dnn`, `cuda`, `sycl`, `hip`, `vulkan`, `metal`, `mcp`, `build`, `api`, `fork-local` |
| [ADR-0375](0375-hip-batch3-float-moment-float-ssim.md) | HIP batch-3 — `float_moment_hip` and `float_ssim_hip` real kernels | Accepted | `hip`, `gpu`, `build`, `feature-extractor`, `fork-local` |
| [ADR-0376](0376-vulkan-void-to-int-buffer-invalidate.md) | Fix silent error-swallow in Vulkan buffer-invalidate readback functions | Accepted | `vulkan`, `gpu`, `build`, `correctness`, `fork-local` |
| [ADR-0377](0377-hip-batch4-ciede-motion-v2.md) | HIP batch-4 — `ciede_hip` and `integer_motion_v2_hip` real kernels | Accepted | `hip`, `gpu`, `build`, `feature-extractor`, `fork-local` |
| [ADR-0378](0378-picture-stream-non-blocking.md) | Per-picture CUDA streams must use CU_STREAM_NON_BLOCKING | Accepted | cuda, performance, gpu, feature-extractor, fork-local |
| [ADR-0379](0379-libvmaf-symbol-visibility.md) | libvmaf Symbol Visibility — Hide Internal Symbols with `-fvisibility=hidden` | Accepted | `build`, `api`, `security`, `abi`, `fork-local` |
| [ADR-0380](0380-ffmpeg-patches-hip-backend-selector.md) | FFmpeg libvmaf filter — HIP backend selector patch (0011) | Accepted | ffmpeg-patches, hip, integration |
| [ADR-0381](0381-vulkan-vif-scale-precision.md) | Fix Vulkan VIF Scale 2/3 Numerical Saturation (PR #718) | Accepted | `vulkan`, `precision`, `build` |
| [ADR-0382](0382-y4m-neg-dimension-rejection.md) | Y4M header parser — reject non-positive width or height before allocation | Accepted | `security`, `fuzz`, `parser`, `fork-local` |
| [ADR-0383](0383-k150k-parallel-cpu-driver.md) | K150K corpus scoring driver — parallel CPU worker redesign | Accepted | `ai`, `corpus`, `performance`, `training`, `fork-local` |
| [ADR-0384](0384-shfmt-src-hook-and-cache-key-fix.md) | Switch shfmt pre-commit hook from binary download to Go-source build | Accepted | `ci`, `build`, `fork-local` |
| [ADR-0385](0385-fex-dedup-by-provided-feature.md) | Feature-extractor deduplication by provided-feature names | Accepted | `correctness`, `cuda`, `gpu`, `feature-extractor`, `fork-local` |
| [ADR-0386](0386-adr-numbering-collision-prevention.md) | ADR Number Collision Prevention — Hook + CI Gate + Helper Script | Accepted | `ci`, `docs`, `git`, `agents` |
| [ADR-0387](0387-renovate-github-app-migration.md) | Migrate Renovate from self-hosted workflow to GitHub App | Accepted | `infra`, `dependency-bot`, `fork-local` |
| [ADR-0388](0388-bristol-bvi-cc-ingest.md) | Ingest BVI-CC as the second tiny-AI training corpus | Draft | ai, fr-regressor, corpus, license, bristol |
| [ADR-0389](0389-vmaf-tiny-v3-mlp-medium.md) | vmaf_tiny_v3 — wider/deeper mlp_medium tiny VMAF MLP | Accepted | ai, dnn, tiny-ai, model, registry, fork-local |
| [ADR-0390](0390-vmaf-tiny-v4-mlp-large.md) | vmaf_tiny_v4 — mlp_large arch (opt-in only; arch ladder stops here) | Accepted | `ai`, `tiny-ai`, `model`, `inference` |
| [ADR-0391](0391-ciede-vulkan-nvidia-f32-f64-precision-gap.md) | ciede2000 Vulkan NVIDIA places=4 fork debt is a structural f32/f64 precision gap | Accepted | vulkan, ciede, precision, gpu, nvidia, fork-local |
| [ADR-0392](0392-vmaf-tune-phase-d-per-shot.md) | `vmaf-tune` Phase D — per-shot CRF tuning | Accepted (CLI bisect wiring landed 2026-05-14; native | tooling, ai, ffmpeg, codec, automation, fork-local |
| [ADR-0393](0393-fr-regressor-v2-probabilistic.md) | `fr_regressor_v2` probabilistic head — deep-ensemble + conformal scaffold | Accepted | ai, fr-regressor, probabilistic, ensemble, conformal, fork-local |
| [ADR-0394](0394-local-sidecar-training.md) | Local sidecar training — on-host bias-correction model | Accepted | ai, vmaf-tune, sidecar, online-learning, privacy, fork-local |
| [ADR-0395](0395-predictor-stub-models-policy.md) | predictor stub-models policy | Accepted | ai, vmaf-tune, predictor, models, fork-local |
| [ADR-0396](0396-video-saliency-extension.md) | Video-temporal saliency extension to `saliency_student_v1` | Accepted | ai, dnn, saliency, video-saliency, vmaf-tune, roi, fork-local, design |
| [ADR-0397](0397-vmaf-tune-phase-f-auto.md) | `vmaf-tune` Phase F — `auto` adaptive recipe-aware tuning | Accepted | tooling, automation, vmaf-tune, ffmpeg, codec, fork-local |
| [ADR-0398](0398-mytestcase-migration-partial-port.md) | MyTestCase upstream migration — partial port (golden-pinned files deferred) | Accepted | testing, upstream-sync, python |
| [ADR-0399](0399-vmaftune-codec-adapter-runtime-contract.md) | `vmaf-tune` codec-adapter contract becomes a runtime contract (HP-1) | Accepted | tooling, codec, automation, fork-local, bug-fix |
| [ADR-0400](0400-encoder-internal-stats-capture.md) | encoder-internal-stats capture (corpus expansion v1) | Accepted | vmaf-tune, corpus, predictor, x264 |
| [ADR-0401](0401-libvmaf-wasm-target.md) | libvmaf WebAssembly target — phased EXPERIMENT then GO | Proposed | build, wasm, browser, ai, fork-local |
| [ADR-0402](0402-mcp-runtime-v2.md) | MCP runtime v2 — UDS transport + real `compute_vmaf` binding | Accepted | mcp, agents, api, transport, fork-local |
| [ADR-0403](0403-mkdocs-strict-gate-validation-policy.md) | mkdocs `--strict` validation policy — actionable carve-outs | Accepted | docs, ci, mkdocs, fork-local |
| [ADR-0404](0404-nightly-fuzz-triage-keep-gates.md) | Keep `nightly.yml` + `fuzz.yml` red until underlying bugs land | Accepted | ci, testing, fuzzing, security |
| [ADR-0405](0405-openvino-npu-ep-wiring.md) | Wire OpenVINO NPU execution provider into the tiny-AI dispatch layer | Accepted | ai, dnn, openvino, intel-ai-pc, fork-local |
| [ADR-0406](0406-sycl-adm-dwt-group-load-deferral.md) | Defer SYCL ADM DWT `group_load` rewrite — divisibility blocker | Accepted | sycl, adm, perf, deferred, fork-local |
| [ADR-0407](0407-adaptivecpp-second-sycl-toolchain.md) | AdaptiveCpp as a second SYCL toolchain | Accepted | sycl, build, toolchain, fork-local, ci, contributor-experience |
| [ADR-0408](0408-ffmpeg-libvmaf-cuda-backend-selector.md) | FFmpeg libvmaf filter — CUDA backend selector | Accepted | ffmpeg-patches, cuda, integration |
| [ADR-0409](0409-ffmpeg-patches-surface-gate.md) | Automated CI gate for the ffmpeg-patches surface-sync rule (CLAUDE.md §12 r14) | Accepted | `ci`, `ffmpeg-integration`, `process`, `rule-enforcement` |
| [ADR-0410](0410-ssimulacra2-cuda-leaks-perf.md) | `ssimulacra2_cuda` GPU module leak + per-scale `malloc` removal | Accepted | cuda, gpu, perf, memory-leak, ssimulacra2, fork-local |
| [ADR-0412](0412-u2netp-fork-mirror-scaffold.md) | Fork-local release-artefact mirror scaffold for `u2netp.pth` (Apache-2.0) | Accepted | ai, dnn, u2netp, saliency, license, apache-2.0, supply-chain, fork-local, docs |
| [ADR-0413](0413-youtube-ugc-corpus-ingestion.md) | YouTube UGC corpus ingestion for `nr_metric_v1` | Accepted | ai, training, corpus, license, fork-local |
| [ADR-0414](0414-saliency-roi-x265-svtav1-vvenc.md) | Saliency-aware ROI for x265 / SVT-AV1 / libvvenc adapters | Accepted | `vmaf-tune`, `saliency`, `codec-adapter`, `roi`, `fork-local` |
| [ADR-0415](0415-cambi-sycl-port.md) | CAMBI SYCL port — closes last CUDA-to-SYCL parity gap | Accepted | `sycl`, `gpu`, `cambi`, `feature-extractor`, `fork-local`, `t3-15` |
| [ADR-0416](0416-vif-upstream-onthefly-filter-sync.md) | VIF on-the-fly filter sync from Netflix upstream | Accepted | vif, upstream-sync, fork-local, netflix-golden, fork-internal |
| [ADR-0417](0417-tiny-ai-netflix-training-scaffold-pr.md) | Tiny-AI Netflix corpus training scaffold — draft PR registration | Accepted | `ai`, `training`, `mcp`, `fork-local`, `onnx`, `docs` |
| [ADR-0418](0418-macos-test-recal-post-vif-sync.md) | Full upstream ADM + VIF-prescale sync (companion to PR #758 / ADR-0416) | Accepted | adm, vif, prescale, upstream-sync, fork-local, netflix-golden, fork-internal |
| [ADR-0419](0419-sve2-probe-darwin-gate.md) | Gate SVE2 build probe to non-Darwin hosts | Accepted | `build`, `simd`, `macos`, `arm64` |
| [ADR-0420](0420-metal-backend-runtime-t8-1b.md) | Metal backend runtime (T8-1b) | Accepted | `gpu`, `metal`, `apple-silicon`, `runtime`, `fork-local` |
| [ADR-0421](0421-metal-first-kernel-motion-v2.md) | Metal first kernel — `integer_motion_v2` (T8-1c) | Accepted | `gpu`, `metal`, `apple-silicon`, `kernel`, `bit-exact`, `fork-local` |
| [ADR-0422](0422-cli-hip-metal-backend-selectors.md) | CLI HIP and Metal Backend Selectors | Accepted | `cli`, `hip`, `metal`, `gpu`, `fork-local` |
| [ADR-0423](0423-metal-iosurface-import-scaffold.md) | Metal IOSurface zero-copy import (T8-IOS) | Accepted | metal, ffmpeg-patches, gpu, t8-ios |
| [ADR-0424](0424-vmaf-tune-corpus-benchmark.md) | `vmaf-tune benchmark` consumes Phase-A corpora | Accepted | vmaf-tune, cli, benchmark, corpus |
| [ADR-0425](0425-vmaf-roi-score-saliency-materialiser.md) | vmaf-roi-score saliency materialiser | Accepted | tooling, ai, saliency, vmaf, fork-local |
| [ADR-0426](0426-chug-hdr-corpus-ingestion.md) | CHUG HDR corpus ingestion | Accepted | ai, hdr, corpus, mos, training, license |
| [ADR-0427](0427-chug-hdr-feature-materialisation.md) | Materialise CHUG HDR Features With Reference-Aligned Pairs | Accepted | ai, hdr, corpus, mos |
| [ADR-0428](0428-vmaf-tune-auto-winner-selection.md) | vmaf-tune auto selects one winner | Accepted | vmaf-tune, cli, planning |
| [ADR-0429](0429-testdata-bench-perf-portability.md) | testdata bench_perf is configurable | Accepted | benchmarks, testdata, tooling |
| [ADR-0430](0430-saliency-rgb-ingest-and-ssimulacra2-docs.md) | Saliency RGB ingest and SSIMULACRA2 public docs | Accepted | vmaf-tune, saliency, docs, metrics, fork-local |
| [ADR-0431](0431-fr-from-nr-cuda-feature-split.md) | Split CUDA and CPU Feature Passes for FR-from-NR Extraction | Accepted | ai, cuda, training-data, corpus, fork-local |
| [ADR-0432](0432-roi-score-high-bit-depth-mask.md) | High-Bit-Depth ROI-Score Mask Materialisation | Accepted | roi, tiny-ai, hdr, tooling, fork-local |
| [ADR-0433](0433-chug-content-splits-and-hdr-audit.md) | CHUG Content Splits And HDR Audit | Accepted | ai, hdr, chug, training, fork-local |
| [ADR-0434](0434-chug-parquet-metadata-enrichment.md) | CHUG Parquet Metadata Enrichment | Accepted | ai, hdr, chug, training, fork-local |
| [ADR-0435](0435-pr-body-pre-push-validation.md) | PR-body pre-push validation hook | Accepted | `ci`, `agents`, `hooks`, `docs` |
| [ADR-0436](0436-mcp-backend-selector-parity.md) | MCP server backend-selector parity | Accepted | mcp, agents, api, dispatch, fork-local |
| [ADR-0437](0437-metal-public-header-install-and-import-state-declaration.md) | Metal public-header install and `vmaf_metal_import_state` declaration | Accepted | metal, build, c-api, install, apple-silicon, fork-local |
| [ADR-0438](0438-cli-parse-short-opt-handler-coverage.md) | CLI parser short-option handler coverage invariant | Accepted | `cli`, `lint`, `testing`, `correctness` |
| [ADR-0444](0444-saliency-student-v2-production-promotion.md) | Promote `saliency_student_v2` to production default | Accepted | `ai`, `dnn`, `saliency`, `tiny-ai`, `fork-local` |
| [ADR-0445](0445-vulkan-pipeline-cache-persistence.md) | Persistent VkPipelineCache for Vulkan compute backend | Accepted | `vulkan`, `gpu`, `performance`, `pipeline-cache`, `fork-local` |
| [ADR-0446](0446-extractor-hdr-and-hfr-feature-options.md) | K150K/CHUG extractor passes HDR and HFR per-feature options | Accepted | ai, hdr, hfr, training, corpus, fork-local |
| [ADR-0447](0447-motion-hfr-under-report.md) | Motion features under-report on HFR / 50p content | Accepted | `ai`, `motion`, `hfr`, `feature-extractor`, `cuda`, `sycl`, `vulkan`, `fork-local` |
| [ADR-0448](0448-active-upstream-monitoring-discipline.md) | Active upstream monitoring (no silent "wait" deferrals) | Accepted | ci, governance, upstream-sync, deferral, fork-local |
| [ADR-0451](0451-local-dev-mcp-container.md) | Local dev-MCP container for live probing | Accepted | `infra`, `docker`, `mcp`, `gpu`, `hip`, `cuda`, `sycl`, `vulkan`, `dev`, `fork-local` |
| [ADR-0452](0452-cambi-calculate-c-values-avx512-neon.md) | Port `calculate_c_values_row` to AVX-512 and NEON | Accepted | `simd`, `cambi`, `perf` |
| [ADR-0453](0453-psnr-enable-chroma-gpu-parity.md) | PSNR `enable_chroma` option parity across all GPU backends | Accepted | cuda, sycl, vulkan, psnr, option-parity, bug |
| [ADR-0454](0454-vif-cuda-smem-staging.md) | VIF CUDA shared-memory staging for horizontal and vertical filter passes | Proposed | `cuda`, `gpu`, `vif`, `performance`, `smem`, `fork-local` |
| [ADR-0455](0455-k150k-split-trainer-promotion.md) | KonViD-150k k150ka/k150kb split promotion into the MOS-head trainer | Accepted | ai, training, corpus, konvid, fork-local |
| [ADR-0456](0456-ssimulacra2-cuda-blur-fusion-transpose.md) | SSIMULACRA2 CUDA Blur: 3-Channel Kernel Fusion and V-Pass Transpose for Coalesced Access | Accepted | `cuda`, `perf`, `ssimulacra2` |
| [ADR-0457](0457-onnx-blobs-to-github-releases.md) | model/tiny/*.onnx blobs ≥1MB live in GitHub Releases, not git | Accepted | ai, model-storage, repo-size, fork-local |
| [ADR-0458](0458-sycl-cambi-ssim-slm-staging.md) | SYCL CAMBI queue-sync collapse + SSIM horizontal SLM staging | Accepted | `sycl`, `perf`, `cambi`, `ssim`, `gpu`, `fork-local` |
| [ADR-0459](0459-vmaftune-panel-aware-recommendations.md) | vmaf-tune panel/display-aware recommendation workstream | Proposed | vmaf-tune, ai, hdr, training, panel, display, fork-local |
| [ADR-0460](0460-dispatch-registry-audit-2026-05-15.md) | Dispatch-strategy registry audit 2026-05-15 | Accepted | dispatch, hip, metal, sycl, vulkan, correctness |
| [ADR-0461](0461-cli-validate-dimensions-chroma.md) | CLI validates positive dimensions and chroma-alignment on input videos | Accepted | `cli`, `validation`, `correctness` |
| [ADR-0463](0463-adm-p-norm-fast-path-vif-arm64-malloc-hoist.md) | ADM p-norm fast-path split and VIF scalar-fallback malloc hoist | Accepted | `perf`, `adm`, `vif`, `simd`, `cpu`, `fork-local` |
| [ADR-0464](0464-cambi-cuda-smem-tile.md) | CAMBI CUDA spatial-mask shared-memory tile | Accepted | `cuda`, `gpu`, `cambi`, `performance`, `kernel`, `fork-local` |
| [ADR-0466](0466-mkdocs-strict-pre-push-hook.md) | mkdocs strict-mode pre-push hook | Accepted | `docs`, `ci`, `git`, `hooks`, `mkdocs` |
| [ADR-0467](0467-ssimulacra2-avx512-neon-ulp-audit.md) | SSIMULACRA2 AVX-512 + NEON IIR Blur / picture_to_linear_rgb ULP Audit — Clean Close | Accepted | `simd`, `ssimulacra2`, `audit` |
| [ADR-0468](0468-hip-float-adm-real-kernel.md) | HIP float_adm real kernel (ninth HIP consumer) | Accepted | `hip`, `build`, `feature-extractor` |
| [ADR-0469](0469-float-psnr-hip-enable-chroma.md) | `float_psnr` HIP twin — wire `enable_chroma` option | Accepted | hip, psnr, option-parity |
| [ADR-0470](0470-vulkan-pipeline-cache.md) | Disk-Persistent VkPipelineCache for Vulkan Feature Extractors | Accepted | `vulkan`, `perf`, `build` |
| [ADR-0471](0471-integer-psnr-hip-enable-chroma.md) | Add `enable_chroma` to `integer_psnr_hip` (chroma parity with CUDA/SYCL/Vulkan twins) | Accepted | hip, psnr, option-parity, chroma, fork-local |
| [ADR-0480](0480-bootstrap-name-builder-dedup.md) | Bootstrap Score Name-Builder Deduplication | Accepted | refactor, predict, libvmaf |
| [ADR-0481](0481-adm-p-norm-hardcoded-deferral.md) | ADM p-norm Parameter Hardcoded at 3.0 — Deferral Decision | Accepted | adm, predict, ai, testing |
| [ADR-0482](0482-vmaf-pre-device-parity.md) | Expand vmaf_pre FFmpeg filter device strings to match full VmafDnnDevice enum | Accepted | `ffmpeg`, `ai`, `build` |
| [ADR-0483](0483-gpu-dispatch-parse-dedup.md) | Extract shared `vmaf_gpu_dispatch_parse_env` tokenizer | Accepted | `cuda`, `sycl`, `vulkan`, `refactor`, `dedup` |
| [ADR-0484](0484-kernel-scaffolding-hip-metal-doc.md) | Extend kernel-scaffolding.md with HIP and Metal lifecycle contract | Accepted | docs, hip, metal, gpu, fork-local |
| [ADR-0485](0485-kernel-lifecycle-zero-dedup.md) | Extract `VMAF_LIFECYCLE_ZERO` macro to eliminate struct-init duplication across HIP and Metal kernel templates | Accepted | `cuda`, `framework`, `lint`, `build` |
| [ADR-0486](0486-context-api-contract-doc.md) | Codify the three-function GPU backend context-API contract in docs | Accepted | `docs`, `gpu`, `hip`, `metal`, `vulkan`, `cuda`, `api`, `fork-local` |
| [ADR-0487](0487-integer-adm-min-val-gpu-parity.md) | Wire adm_min_val option into integer_adm GPU backends | Accepted | `cuda`, `sycl`, `vulkan`, `adm`, `parity` |
| [ADR-0488](0488-gpu-dispatch-env-shared-snapshot.md) | Shared once-snapshot helper for GPU dispatch env variables | Accepted | gpu, cuda, vulkan, sycl, dispatch, threading, refactor, fork-local |
| [ADR-0489](0489-cambi-sycl-event-chain.md) | CAMBI SYCL — Replace GPU-to-GPU `q.wait()` Calls with Event Chains (SY-1) | Accepted | `sycl`, `gpu`, `cambi`, `performance`, `fork-local` |
| [ADR-0490](0490-float-ms-ssim-metal-port.md) | float_ms_ssim Metal port | Accepted | `metal`, `ms-ssim`, `float`, `apple-silicon`, `fork-local` |
| [ADR-0491](0491-motion-dedicated-doc-page.md) | Add dedicated `docs/metrics/motion.md` reference page | Accepted | `docs`, `metrics`, `motion`, `fork-local` |
| [ADR-0492](0492-vulkan-vif-shader-fp64-g-computation.md) | Promote Vulkan VIF g/sv_sq Computation to double Precision | Superseded by [ADR-0512](0512-vulkan-vif-two-variant-shader.md) | `vulkan`, `vif`, `gpu-parity`, `precision` |
| [ADR-0493](0493-test-yuv-fixture-md5-verification.md) | Test YUV fixtures must be md5-verified, not just present-by-name | Accepted | testing, ci, fixtures, golden-data |
| [ADR-0494](0494-python-test-suite-restoration.md) | Restore the non-golden Python test suite to green | Accepted | testing, ci, python, regression-recovery |
| [ADR-0495](0495-mcp-probe-bug-fixes.md) | MCP server probe-driven bug-fix cluster (2026-05-17) | Accepted | mcp, ai, testing, regression-recovery |
| [ADR-0496](0496-prefer-dev-mcp-container-rule.md) | Default to the `vmaf-dev-mcp` container for all vmaf / vmaf-tune / ai / MCP work | Accepted | tooling, container, dev-experience, project-rule, fork-local |
| [ADR-0497](0497-vmaf-tune-bbb-e2e-bug-cluster.md) | vmaf-tune BBB end-to-end bug cluster (compare / ladder / report) | Accepted | `vmaf-tune`, `cli`, `bugfix`, `docs` |
| [ADR-0498](0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md) | vmaf-tune BBB end-to-end v2 bug cluster + explicit-backend semantics | Accepted | `vmaf-tune`, `cli`, `libvmaf`, `bugfix`, `docs`, `container` |
| [ADR-0499](0499-vmaf-tune-ladder-reference-decode-v3.md) | vmaf-tune ladder must decode container/Y4M references before scoring | Accepted | vmaf-tune, ladder, corpus, ffmpeg, docs |
| [ADR-0500](0500-vif-perf-lut-shrink-and-filter-cache.md) | VIF log2 LUT Shrink and Gaussian Filter Cache | Accepted | `simd`, `perf`, `integer-vif`, `float-vif` |
| [ADR-0501](0501-vmaf-tune-bbb-e2e-v4-bug-cluster.md) | vmaf-tune ladder cross-resolution scoring + report degraded flag | Accepted | vmaf-tune, ladder, corpus, report, vmaf-cli, docs |
| [ADR-0502](0502-adm-decouple-gather-prefetch.md) | ADM decouple gather prefetch (Approach B) | Accepted | `simd`, `perf`, `adm`, `avx512`, `fork-local` |
| [ADR-0503](0503-vif-subsample-rd-8-loop-fission.md) | `vif_subsample_rd_8_avx512` Loop Fission to Reduce ZMM Register Spill | Accepted | `simd`, `performance`, `avx512`, `vif` |
| [ADR-0504](0504-float-convolution-avx512-port.md) | AVX-512F port of float separable convolution scanlines | Accepted | `simd`, `performance`, `build` |
| [ADR-0505](0505-vmaf-tune-bbb-e2e-v5-bug-cluster.md) | vmaf-tune ladder container-source encode + full per-CRF sample cloud | Accepted | vmaf-tune, ladder, corpus, encode, vmaf-cli, docs |
| [ADR-0506](0506-vmaf-tune-bbb-e2e-v6-bug-cluster.md) | vmaf-tune ladder duration clipping, raw-YUV cross-res decode, CLI exit code | Accepted | vmaf-tune, ladder, corpus, encode, cli, docs |
| [ADR-0508](0508-vmaf-tune-ladder-pass1-stats-duration-clip.md) | vmaf-tune ladder pass-1 stats argv honours --duration | Accepted | vmaf-tune, ladder, encode, bugfix |
| [ADR-0509](0509-vmaf-tune-compare-container-source-framerate-probe.md) | vmaf-tune compare auto-probes container-source framerate / duration | Accepted | vmaf-tune, compare, bisect, encode, vmaf-cli |
| [ADR-0510](0510-chug-extract-vmaf-alignment-fr-from-nr-guard.md) | CHUG re-extract VMAF-alignment fix — FR-corpus guard on the FR-from-NR extractor | Accepted | ai, corpus, chug, k150k, extractor, training-data, regression-guard |
| [ADR-0511](0511-mcp-backend-probe-allowlist-and-ladder-backend.md) | MCP backend probe, default allowlist, and `vmaf-tune ladder --score-backend` (2026-05-18) | Accepted | mcp, vmaf-tune, ai, dx, bugfix |
| [ADR-0512](0512-vulkan-vif-two-variant-shader.md) | Vulkan VIF Two-Variant Compute Shader (fp32 Auto-Fallback) | Accepted | `vulkan`, `vif`, `gpu-parity`, `precision`, `compatibility` |
| [ADR-0513](0513-per-shot-scene-threshold-and-1-shot-chart.md) | Expose `--scene-threshold` + `--max-shot-duration`; render 1-shot timeline chart | Accepted | vmaf-tune, per-shot, report, ux |
| [ADR-0514](0514-dev-container-full-backend-exposure.md) | dev-MCP container exposes every host GPU backend (CUDA + SYCL + Vulkan + HIP) | Accepted | container, dev-experience, gpu, sycl, vulkan, hip, cuda, fork-local |
| [ADR-0515](0515-test-public-api-score-mingw64-temp-path.md) | Portable temp-path setup for `test_public_api_score` on MinGW64 | Accepted | ci, build, windows, mingw, test, fork-local, bugfix |
| [ADR-0516](0516-vmaf-tune-compare-rate-quality-sweep.md) | `vmaf-tune compare` multi-target rate-quality sweep (schema v2) (2026-05-18) | Accepted | vmaf-tune, ux, dx, schema-evolution |
| [ADR-0517](0517-mcp-run-benchmark-repair.md) | Repair MCP `run_benchmark` tool — drop per-call args, inject VMAF_BIN, guard set -u in bench_all.sh | Accepted | mcp, bugfix, fork-local, benchmark |
| [ADR-0518](0518-tiny-model-loader-external-data-and-feature-rank.md) | Tiny-model loader accepts external-data and feature-vector ONNX | Accepted | `ai`, `dnn`, `loader`, `bug-fix` |
| [ADR-0519](0519-hip-import-state-implementation.md) | Implement vmaf_hip_import_state to unblock --backend hip | Accepted | hip, backend, libvmaf, gpu |
| [ADR-0520](0520-cli-no-reference-wiring.md) | Wire `vmaf --no-reference` through to the scoring path | Accepted | cli, ai, dnn, docs |
| [ADR-0521](0521-msvc-posix-gating-vif-avx512-yuv-input.md) | MSVC portability gating — `vif_avx512.c` noinline/noclone + `yuv_input.c` S_ISREG/fstat | Accepted | `ci`, `build`, `windows`, `msvc`, `simd`, `tools`, `portability`, `fork-local`, `bugfix` |
| [ADR-0522](0522-tiny-codec-preset-crf-cli-flags.md) | `--tiny-codec` / `--tiny-preset` / `--tiny-crf` populate codec one-hot block | Accepted | `cli`, `ai`, `dnn`, `tiny-model` |
| [ADR-0523](0523-hip-integer-motion-extractor-registration.md) | Register `vmaf_fex_integer_motion_hip` in the extractor list | Accepted | `hip`, `gpu`, `feature-extractor`, `bugfix`, `fork-local` |
| [ADR-0524](0524-tiny-model-loader-symbolic-batch-dim.md) | Tiny-model loader accepts symbolic batch dim | Accepted | `ai`, `dnn`, `loader`, `bug-fix` |
| [ADR-0525](0525-aiutils-subprocess-dedup.md) | Extract `run_cmd` subprocess helper into `aiutils` | Accepted | `ai`, `refactor`, `fork-local` |
| [ADR-0526](0526-ms-ssim-sycl-enable-lcs-parity.md) | Add enable_lcs and enable_chroma to float_ms_ssim SYCL twin | Accepted | `sycl`, `parity`, `options` |
| [ADR-0527](0527-bvi-dvc-pre-extracted-dir-input.md) | Accept pre-extracted BVI-DVC YUVs via `--bvi-dir` | Accepted | `ai`, `corpus`, `cli`, `docs` |
| [ADR-0528](0528-cli-parse-test-stderr-pipe-drain-and-error-fallback.md) | `test_cli_parse_long_only_args` stderr-pipe drain + `error()` non-fatal fallback | Accepted | cli, test, regression, fork-local, bugfix |
| [ADR-0529](0529-dev-container-whole-dri-bind.md) | Replace `/dev/dri/by-path` bind with whole `/dev/dri` bind in dev container | Accepted | `build`, `ci`, `cuda`, `sycl`, `agents` |
| [ADR-0530](0530-hip-feature-flag-promotion-and-picture-buffer.md) | HIP feature-extractor flag promotion and HIP_DEVICE picture-buffer type | Accepted | `hip`, `gpu`, `dispatch`, `feature-extractor` |
| [ADR-0531](0531-per-shot-bitrate-kbps-and-last-shot-chart.md) | Per-shot plan emits bitrate_kbps + chart shows last shot | Accepted | `vmaf-tune`, `per-shot`, `report`, `chart` |
| [ADR-0532](0532-per-shot-segments-readonly-cwd.md) | tune-per-shot tolerates read-only CWD when writing segments | Accepted | `vmaf-tune`, `cli`, `robustness`, `container` |
| [ADR-0533](0533-hip-all-extractors-registration-sweep.md) | Full HIP feature-extractor registration sweep | Accepted | `hip`, `gpu`, `feature-extractor`, `bugfix`, `fork-local` |
| [ADR-0534](0534-compare-rate-quality-chart-from-bisect-samples.md) | vmaf-tune compare emits + renders rate-quality curve from per-iteration bisect samples | Accepted (target-VMAF defaults superseded by [ADR-0538](0538-premium-vmaf-target-defaults-and-bisect.md)) | vmaf-tune, compare, report, chart, ux |
| [ADR-0535](0535-adr-atomic-allocator.md) | Atomic ADR Number Allocator with Cross-Branch Claim | Accepted | `ci`, `docs`, `git`, `agents`, `tooling` |
| [ADR-0536](0536-per-shot-bitrate-predicate-chain.md) | Per-shot predicate threads bitrate_kbps through bisect sidecar (PR #1290 follow-up) | Accepted | `vmaf-tune`, `per-shot`, `bugfix` |
| [ADR-0537](0537-hip-integer-vif-kernel-fix.md) | HIP integer VIF kernel crash fix — filter upload, bounds, HtoD staging | Accepted | `hip`, `gpu`, `kernel`, `vif`, `bug-fix` |
| [ADR-0538](0538-premium-vmaf-target-defaults-and-bisect.md) | vmaf-tune compare ships premium-archival --target-vmafs default + bisect reaches VMAF 95+ | Accepted | vmaf-tune, compare, bisect, defaults, premium-archival |
| [ADR-0539](0539-hip-adm-kernels-real.md) | integer ADM HIP kernels — real implementation replacing weak HSACO stubs | Superseded by [ADR-1167](1167-adm-cm-row-level-rounding.md) | hip, gpu, feature, integer-adm, port |
| [ADR-0540](0540-dev-container-ffmpeg-av1-and-hwaccel-encoders.md) | dev-MCP container FFmpeg ships AV1 (SVT/aom) + VVenC + hardware encoders (NVENC, oneVPL/QSV, AMF) | Accepted | container, dev-experience, ffmpeg, codecs, av1, vvc, nvenc, qsv, amf, fork-local |
| [ADR-0541](0541-dev-container-sycl-hip-runtime-fix.md) | Pin dev-MCP container Intel NEO + ROCm runtimes to versions matching the host kernel | Accepted | build, dev, sycl, hip, container, gpu |
| [ADR-0542](0542-dev-container-full-gpu-plumbing.md) | Full GPU backend plumbing in the dev-mcp container | Accepted | `dev-container`, `cuda`, `vulkan`, `sycl`, `hip`, `rocm` |
| [ADR-0543](0543-adr-0498-enforcement-hardening.md) | enforcement hardening — distinct exit code + structured JSON error + per-feature gate | Accepted | `cli`, `libvmaf`, `bugfix`, `backend`, `exit-code`, `extends-adr-0498` |
| [ADR-0544](0544-fix-feature-extractor-list-dedup.md) | deduplicate `feature_extractor_list[]` registrations | Accepted | `bug`, `dispatch`, `vulkan`, `sycl`, `registry` |
| [ADR-0545](0545-wire-or-delete-dead-extractor-files.md) | Wire or delete dead Vulkan/Metal feature-extractor source files | Accepted | `vulkan`, `metal`, `build`, `housekeeping`, `dead-code` |
| [ADR-0546](0546-audit-bundle-vulkan-saliency-modelcard.md) | Audit bundle — Vulkan motion dispatch wiring, saliency hard-fail, model-card placeholder | Accepted | `vulkan`, `vmaf-tune`, `ai`, `build`, `docs` |
| [ADR-0547](0547-ai-script-env-vars.md) | VMAF_&lt;NAME&gt;_DIR env-var overrides for ai/scripts corpus paths + drop cli.py.bak | Accepted | ai, scripts, container, hygiene, fork-local |
| [ADR-0548](0548-vmaf-tune-full-file-and-no-bisect.md) | vmaf-tune tune-per-shot accepts container sources directly; compare gains --no-bisect mode | Accepted | `vmaf-tune`, `cli`, `ergonomics`, `compare`, `per-shot` |
| [ADR-0549](0549-audit-cleanup-bundle-2.md) | Audit cleanup bundle 2 | Accepted | `docs`, `build`, `container`, `housekeeping`, `fork-local` |
| [ADR-0550](0550-tiny-model-auto-resize.md) | Auto-resize input plane to NR tiny-model dims + `--tiny-resize` flag | Accepted | `ai`, `cli`, `dnn`, `api` |
| [ADR-0551](0551-local-explainer-hang-diagnosis.md) | VCQ-223 LocalExplainer CI timeout — root cause and fix path | Proposed | `python`, `test`, `local-explainer`, `performance`, `bugfix`, `fork-local` |
| [ADR-0552](0552-hip-integer-vif-deterministic-reduce.md) | Deterministic wavefront reduction for `integer_vif_hip` horizontal kernels | Accepted | `hip`, `gpu`, `kernel`, `vif`, `parity`, `correctness`, `fork-local` |
| [ADR-0556](0556-python-mcp-ai-audit-2026-05-18.md) | Python / MCP / AI silent-fallback audit fixes (2026-05-18) | Accepted | `python`, `mcp`, `ai`, `vmaf-tune`, `correctness`, `audit` |
| [ADR-0559](0559-feature-coverage-audit.md) | Feature Coverage Audit — Add speed_chroma + speed_temporal to Extraction Scripts (HDR-model prep) | Accepted | ai, feature-extraction, speed, hdr, corpus, fork-local |
| [ADR-0561](0561-hip-gfx-targets-fallback-widening.md) | Widen HIP `gfx_targets` hardcoded fallback | Accepted | not recorded |
| [ADR-0562](0562-local-explainer-hang-fix.md) | VCQ-223 LocalExplainer hang fix — cap neighbor_samples in test runner | Accepted | `python`, `test`, `local-explainer`, `performance`, `bugfix`, `fork-local` |
| [ADR-0563](0563-hip-extractor-audit-verification.md) | HIP extractor audit — verification of 9 remaining scaffold claims | Accepted | `hip`, `gpu`, `audit`, `parity`, `fork-local` |
| [ADR-0564](0564-integer-ssim-gpu-real-kernels.md) | Real integer_ssim GPU kernels (CUDA, HIP, SYCL) — replace silent float_ssim substitution | Accepted | `cuda`, `hip`, `sycl`, `ssim`, `kernel`, `correctness`, `gpu`, `fork-local` |
| [ADR-0565](0565-continuous-feature-mix-eval-pipeline.md) | Continuous Feature-Mix Evaluation Pipeline (predictor-bench) | Proposed | ai, vmaf-tune, predictor, eval, corpus, fork-local, ci |
| [ADR-0566](0566-hip-vif-per-feature-places4-gate.md) | HIP VIF per-feature parity gate: places=4 (supersedes ADR-0537 §follow-up) | Accepted | not recorded |
| [ADR-0567](0567-speed-chroma-temporal-real-gpu.md) | Real On-Device GPU Kernels for speed_chroma and speed_temporal (4 Backends) | Accepted | `cuda`, `sycl`, `hip`, `vulkan`, `speed`, `feature`, `gpu`, `fork-local` |
| [ADR-0568](0568-sycl-icpx-aot-targets-default.md) | Default `sycl_icpx_aot_targets` to full Intel arch list | Accepted | `sycl`, `build`, `meson`, `gpu`, `intel`, `aot`, `fork-local` |
| [ADR-0569](0569-sdk-version-bumps-2026-05-18.md) | SDK / Tool Version Bumps — 2026-05-18 | Accepted | `build`, `container`, `ci`, `deps`, `pre-commit`, `fork-local` |
| [ADR-0573](0573-dev-container-ubuntu-26-04-with-cuda-13-2.md) | Dev-mcp container — ubuntu:26.04 + CUDA 13.2 + hipcc + ocloc | Superseded by [ADR-0738](0738-bump-cuda-133-r610-local.md) | `build`, `ci`, `cuda`, `container`, `dev` |
| [ADR-0574](0574-hdr-features-cuda-twins-phase-1.md) | CUDA Twins for HDR-Model Features — Phase 1 (aim, adm3) | Accepted | `cuda`, `feature`, `hdr`, `adm` |
| [ADR-0575](0575-windows-msvc-stat-compat-include-order.md) | Fix yuv_input.c stat compat — include-order and `_MSC_VER` guard | Accepted | `ci`, `build`, `windows`, `msvc`, `mingw`, `tools`, `portability`, `bugfix`, `fork-local` |
| [ADR-0576](0576-ffmpeg-patches-n811-full-feature-exposure.md) | ffmpeg-patches n8.1.1 full-feature-exposure sync | Accepted | `ffmpeg`, `build`, `ci`, `cuda`, `sycl`, `hip`, `vulkan`, `metal` |
| [ADR-0577](0577-vmaftune-bisect-concurrency-cap-and-aggressive-cleanup.md) | vmaf-tune bisect decode concurrency cap and aggressive workdir cleanup | Accepted | `vmaf-tune`, `compare`, `bisect`, `disk-space`, `concurrency`, `fork-local` |
| [ADR-0578](0578-vif-scratch-buf-hoist-to-vifstate.md) | Hoist VIF scratch buffer from per-frame allocation to VifState | Accepted | perf, vif, cpu, build |
| [ADR-0579](0579-vmaf-tune-auto-execute-mode.md) | `vmaf-tune auto --execute` — Phase F real encode/score execution mode | Accepted | `vmaf-tune`, `phase-f`, `encode`, `score`, `cli`, `fork-local` |
| [ADR-0580](0580-float-ansnr-enable-chroma.md) | float_ansnr enable_chroma option | Accepted | `feature-extractor`, `metrics` |
| [ADR-0581](0581-integer-vif-enable-chroma.md) | Add `enable_chroma` option to `integer_vif` | Accepted | `feature`, `vif`, `chroma` |
| [ADR-0582](0582-ms-ssim-enable-db-clip-db-gpu-parity.md) | MS-SSIM `enable_db` and `clip_db` option parity on CUDA and SYCL backends | Accepted | cuda, sycl, ms_ssim, option-parity, bug |
| [ADR-0583](0583-float-ms-ssim-enable-chroma.md) | Add `enable_chroma` option to the `float_ms_ssim` extractor | Accepted | ms-ssim, float-ms-ssim, option-parity, metrics, correctness, fork-local |
| [ADR-0584](0584-moment-sve2-port.md) | `float_moment` SVE2 port | Accepted | arm64, sve2, simd, float_moment, bit-exactness |
| [ADR-0585](0585-psnr-hvs-vulkan-enable-chroma.md) | Add `enable_chroma` option to `psnr_hvs_vulkan` | Accepted | psnr-hvs, vulkan, option-parity, metrics, fork-local |
| [ADR-0586](0586-integer-adm-vulkan-canonical-rename.md) | Introduce integer_adm_vulkan.c as canonical Vulkan integer ADM extractor | Accepted | `vulkan`, `build`, `feature-extractor` |
| [ADR-0587](0587-metal-cambi-real-kernel.md) | Real Metal Compute Kernels for CAMBI | Accepted | `metal`, `cambi`, `gpu`, `build` |
| [ADR-0588](0588-vmaf-tune-executor-pershot-saliency.md) | vmaf-tune executor — per-shot and saliency execution modes | Accepted | `vmaf-tune`, `executor`, `per-shot`, `saliency`, `phase-f`, `fork-local` |
| [ADR-0589](0589-metal-ssim-lcs-db-parity.md) | Metal `float_ssim` option parity — `enable_lcs`, `enable_db`, `clip_db`, `scale` | Accepted | `metal`, `ssim`, `option-parity`, `apple-silicon`, `kernel`, `fork-local` |
| [ADR-0590](0590-ms-ssim-enable-db-gpu-parity.md) | Wire `enable_db` / `clip_db` into the CUDA and SYCL MS-SSIM twins | Accepted | cuda, sycl, ms-ssim, option-parity, bug, fork-local |
| [ADR-0591](0591-restore-rfe-hw-flags-cache.md) | Restore `rfe_hw_flags` per-frame bitmask cache after PR #1067 clobber | Accepted | `cuda`, `perf`, `bug`, `libvmaf` |
| [ADR-0592](0592-hip-float-vif-stub-removal.md) | Remove float_vif_score weak HSACO stub now that real HIP kernel ships | Accepted | hip, build, cleanup |
| [ADR-0593](0593-hip-psnr-moment-kernels-real.md) | HIP integer_moment kernel — register real HSACO blob alongside psnr / psnr_hvs | Accepted | hip, gpu, parity, build |
| [ADR-0594](0594-hip-ssimulacra2-blur-fp-contract-off.md) | Per-kernel `hip_cu_extra_flags` dispatch — disable FMA contraction for `ssimulacra2_blur` HIP HSACO | Accepted | `hip`, `build`, `ssimulacra2`, `numerics` |
| [ADR-0595](0595-codec-adapter-two-pass-real.md) | Real two-pass argv for all 14 codec adapters | Accepted | vmaf-tune, codec, encode, ffmpeg |
| [ADR-0596](0596-hip-cuda-dead-tu-cleanup.md) | Delete orphan and duplicate HIP/CUDA translation units | Accepted | `hip`, `cuda`, `build`, `cleanup` |
| [ADR-0597](0597-integer-vif-luma-only-clarification.md) | `integer_vif` is luma-only across every backend; CUDA `enable_chroma` is a documented no-op | Accepted | cuda, vif, parity, docs, audit-disposition |
| [ADR-0598](0598-vmaftune-workdir-relocation.md) | vmaf-tune workdir relocation — disk-space preflight + VMAFTUNE_WORKDIR env var | Accepted | `vmaf-tune`, `bugfix`, `cli`, `container`, `workspace` |
| [ADR-0599](0599-cross-backend-parity-matrix-2026-05-18.md) | Cross-Backend Parity Audit — Full Extractor Matrix (2026-05-18) | Accepted | `cuda`, `sycl`, `vulkan`, `hip`, `ci`, `parity`, `audit` |
| [ADR-0600](0600-upstream-port-direct-read.md) | Port upstream USE_DIRECT_READ zero-copy input path (Netflix/vmaf@30a6e2a8d) | Accepted | `upstream-port`, `performance`, `tools`, `cli`, `build` |
| [ADR-0601](0601-vmaftune-qsv-amf-hw-init-and-probe-fix.md) | vmaf-tune QSV/AMF hardware-device init + encoder probe size fix | Accepted | `vmaf-tune`, `compare`, `qsv`, `amf`, `nvenc`, `hardware`, `probe`, `bugfix`, `fork-local` |
| [ADR-0602](0602-macos-vmaf-write-output-segv.md) | macOS SIGSEGV in vmaf_write_output — pic_cnt underflow + missing vmaf NULL guard | Accepted | `bugfix`, `macos`, `output`, `portability`, `correctness`, `fork-local` |
| [ADR-0603](0603-ubuntu-26-04-fallout-fixes.md) | Ubuntu 26.04 (Resolute Raccoon) fallout fixes — CUDA 13.2, Python 3.14, apt renames | Accepted | `build`, `cuda`, `ci`, `python`, `supply-chain` |
| [ADR-0604](0604-rocm-renovate-manager.md) | Add Renovate customManager for ROCm apt-repo tracking | Accepted | `build`, `container`, `supply-chain`, `hip`, `renovate` |
| [ADR-0605](0605-renovate-custommgr-dev-image.md) | Renovate customManagers for all dev/Containerfile pinned dependencies | Accepted | `build`, `container`, `supply-chain`, `renovate`, `cuda`, `sycl`, `hip`, `intel`, `onnx` |
| [ADR-0606](0606-macos-vmaf-write-output-segv-deep-fix.md) | macOS SIGSEGV deep-fix in output.c writers (PR #1403 follow-up) | Accepted | `bugfix`, `macos`, `output`, `portability`, `correctness`, `fork-local` |
| [ADR-0607](0607-vmaftune-shared-ref-yuv-decode-once.md) | vmaf-tune compare: decode reference YUV once for the entire run | Accepted | `vmaf-tune`, `performance`, `disk-space`, `compare` |
| [ADR-0608](0608-zed-editor-project-config.md) | Commit `.zed/` project configuration for Zed editor parity with `.vscode/` | Accepted | `dev`, `ide`, `docs`, `build`, `workspace` |
| [ADR-0612](0612-tiny-ai-netflix-training-scaffold-2026-05-19.md) | Tiny-AI training on the original Netflix VMAF training corpus | Proposed | `ai`, `docs`, `workspace`, `mcp` |
| [ADR-0613](0613-dynamic-optimizer.md) | Dynamic Optimizer — Joint Shot-Boundary + CRF Co-Optimisation | Proposed | `ai`, `planning`, `vmaf-tune` |
| [ADR-0614](0614-per-shot-abr-rendition.md) | Per-Shot ABR Rendition Selection | Proposed | `ai`, `planning`, `vmaf-tune` |
| [ADR-0615](0615-fast-nr-prescoring.md) | Fast NR Pre-Scoring for CRF Bisect Acceleration | Proposed | `ai`, `planning`, `vmaf-tune` |
| [ADR-0616](0616-vmaf-neg-integration.md) | VMAF NEG Integration into vmaf-tune | Proposed | `ai`, `planning`, `vmaf-tune`, `docs` |
| [ADR-0617](0617-cross-shot-complexity-weighting.md) | Cross-Shot Complexity Weighting and Title-Level Quality Constraints | Proposed | `ai`, `planning`, `vmaf-tune` |
| [ADR-0618](0618-content-aware-classifier.md) | Content-Aware Classifier for Encoder Routing | Proposed | `ai`, `planning`, `vmaf-tune`, `dnn` |
| [ADR-0620](0620-scaffold-audit-p0-silent-correctness-fixes.md) | Scaffold audit P0 — three silent-correctness fixes | Accepted | `python`, `correctness`, `bugfix`, `fork-local` |
| [ADR-0621](0621-scaffold-audit-p3-cleanup.md) | Scaffold Audit P3 — six cleanup items + state drift | Accepted | hygiene, ai, python, test, docs, ci, fork-local |
| [ADR-0622](0622-vmaf-neg-integration-impl.md) | VMAF NEG Integration Implementation | Accepted (Implemented) | `vmaf-tune`, `neg`, `docs` |
| [ADR-0623](0623-scaffold-audit-p2-half-finished.md) | Scaffold audit P2 — half-finished implementation fixes | Accepted | `ci`, `build`, `docs`, `hip`, `adm`, `state` |
| [ADR-0624](0624-fast-nr-prescoring-impl.md) | Fast NR Pre-Scoring Implementation (ADR-0615 impl) | Accepted (Implemented) | `ai`, `vmaf-tune`, `bisect`, `onnx`, `fork-local` |
| [ADR-0626](0626-macos-ci-tmate-debug-on-failure.md) | SSH-into-runner debug session on macOS CI failure via tmate | Accepted | ci, macos, debug, fork-local |
| [ADR-0628](0628-adr-allocator-remote-aware.md) | Remote-aware ADR number allocator — cross-worktree collision prevention | Accepted | adr, tooling, ci, governance, agents, fork-local |
| [ADR-0634](0634-mcp-p0-iserror-and-probe-version-encoded.md) | MCP P0 fixes — isError spec bug, probe_backend, vmaf_version, vmaf_score_encoded | Accepted | `mcp`, `bugfix`, `spec-correctness`, `fork-local` |
| [ADR-0635](0635-ci-warning-omnibus-2026-05-19.md) | CI Warning Omnibus (2026-05-19) | Accepted | not recorded |
| [ADR-0637](0637-ci-test-failures-omnibus.md) | Fix 5 master CI failures — MCP smoke syntax, coverage floor, and job timeouts | Accepted | `ci`, `mcp`, `coverage`, `vulkan`, `timeout`, `fork-local` |
| [ADR-0638](0638-mcp-p1-vmaftune-extractors-models-progress.md) | MCP P1 surface — vmaf-tune integration, list_extractors, describe_model, progress notifications | Accepted | `mcp`, `vmaf-tune`, `api`, `docs` |
| [ADR-0639](0639-scaffold-audit-p1-feature-plumbing-fixes.md) | Scaffold-audit P1 — backend precheck, HIP picture, mobilesal bpc, DNN multi-output | Accepted | `python`, `hip`, `ai`, `dnn`, `docs`, `vmaf-tune` |
| [ADR-0640](0640-tiny-ai-netflix-training-scaffold-2026-05-20.md) | Tiny-AI training on the original Netflix VMAF training corpus (2026-05-20 scaffold iteration) | Proposed | `ai`, `docs`, `workspace`, `mcp` |
| [ADR-0641](0641-dev-container-encoder-probe-hardening.md) | Harden dev-container encoder probes and compare reports | Accepted | `dev-container`, `vmaf-tune`, `ffmpeg`, `qsv`, `amf`, `reports`, `fork-local` |
| [ADR-0642](0642-ai-refresh-full-feature-defaults.md) | AI refresh defaults use current fork full-feature extractors | Accepted | `ai`, `training-data`, `full-features`, `konvid`, `ugc`, `bvi-dvc`, `fork-local` |
| [ADR-0643](0643-vmaf-tune-encoder-profile-contract.md) | &lt;fill in title&gt; | Proposed | &lt;fill in&gt; |
| [ADR-0644](0644-vmaf-tune-codec-runtime-variants.md) | Add vmaf-tune codec runtime variants | Accepted | `vmaf-tune`, `ffmpeg`, `cli`, `docs`, `fork-local` |
| [ADR-0645](0645-integer-adm-pnorm-simd.md) | Thread integer ADM p-norm through SIMD callbacks | Accepted | simd, feature-extractor, testing |
| [ADR-0646](0646-dnn-attached-multi-output.md) | Route Attached DNN Multi-Output Tensors | Accepted | ai, dnn, api |
| [ADR-0647](0647-ai-fr-regressor-v1-refresh-20260520.md) | Refresh `fr_regressor_v1` from the 2026-05-20 Netflix feature table | Accepted | ai, tiny-ai, model-refresh, netflix-public, fr-regressor, fork-local |
| [ADR-0648](0648-mos-head-feature-jsonl-chug-id.md) | CHUG HDR MOS Trainer Entry Point | Proposed | ai, hdr, chug, mos, training |
| [ADR-0649](0649-chug-hdr-wide-mos-feature-schema.md) | CHUG HDR Wide MOS Feature Schema | Proposed | ai, hdr, chug, mos, training |
| [ADR-0650](0650-signal-mix-audit.md) | Add a Signal-Mix Audit CLI | Accepted | ai, metrics, audit, hdr |
| [ADR-0651](0651-chug-hdr-row-metadata.md) | Preserve CHUG HDR Metadata On Feature Rows | Accepted | ai, chug, hdr, metadata, training |
| [ADR-0652](0652-chug-visual-signal-primitives.md) | Add CHUG Visual-Signal Primitives | Accepted | ai, chug, hdr, features, training |
| [ADR-0653](0653-chug-display-profile-training.md) | CHUG Display Profile Training | Proposed | ai, hdr, chug, mos, training |
| [ADR-0654](0654-predictor-saliency-signals.md) | Predictor Preserves Saliency Signals | Accepted | ai, vmaf-tune, saliency, predictor |
| [ADR-0655](0655-saliency-feature-materializer.md) | Saliency Feature Materializer | Accepted | ai, saliency, training-data, docs |
| [ADR-0656](0656-external-bench-wrapper-schema.md) | External-bench wrappers emit registry competitor keys | Accepted | ai, testing, tooling, fork-local |
| [ADR-0657](0657-second-opinion-feature-materializer.md) | Second-Opinion Feature Materializer | Accepted | ai, signal-mix, mos, external-bench |
| [ADR-0658](0658-project-modernization-audit.md) | Project modernization audit | Accepted | ai, tooling, docs, backlog |
| [ADR-0659](0659-modernization-audit-false-positive-filter.md) | Modernization audit false-positive filter | Accepted | developer-tools, backlog, docs, fork-local |
| [ADR-0660](0660-tiny-ai-disabled-runtime-gate.md) | Tiny-AI extractors check DNN availability before model paths | Accepted | `tiny-ai`, `dnn`, `feature-extractors`, `api`, `docs` |
| [ADR-0661](0661-ai-run-manifest-provenance.md) | AI run manifest provenance | Accepted | ai, tooling, manifests, training |
| [ADR-0662](0662-vulkan-motion-lavapipe-parity.md) | Vulkan Motion Lavapipe Parity | Accepted | vulkan, cuda, sycl, ci, feature-extractor, numerical-correctness |
| [ADR-0663](0663-mos-label-materializer.md) | MOS Label Materializer | Accepted | ai, mos, training, corpus |
| [ADR-0664](0664-windows-cuda-toolkit-installer.md) | Install Windows CUDA directly in CI | Accepted | ci, build, cuda, windows, github-actions |
| [ADR-0665](0665-fast-nr-calibration-quality-guard.md) | &lt;fill in title&gt; | Proposed | &lt;fill in&gt; |
| [ADR-0666](0666-tune-report-quick-takeaways.md) | &lt;fill in title&gt; | Proposed | &lt;fill in&gt; |
| [ADR-0667](0667-vmaf-tune-score-backend-native-priority.md) | vmaf-tune score backend native priority | Accepted | vmaf-tune, gpu, cuda, sycl, hip, vulkan, fork-local |
| [ADR-0668](0668-ai-derived-table-provenance.md) | AI Derived Table Provenance | Proposed | ai, training, provenance, parquet |
| [ADR-0669](0669-ai-corpus-jsonl-provenance.md) | AI Corpus JSONL Provenance | Proposed | ai, training, provenance, corpus |
| [ADR-0670](0670-ai-legacy-corpus-extraction-manifests.md) | AI Legacy Corpus Extraction Manifests | Proposed | ai, training, provenance, corpus |
| [ADR-0671](0671-u2netp-mirror-exporter.md) | U2NetP Mirror Exporter | Accepted | ai, dnn, u2netp, saliency, onnx, provenance, fork-local |
| [ADR-0672](0672-saliency-materializer-temporal-controls.md) | Saliency Materializer Temporal Controls | Accepted | ai, saliency, materializer, provenance, fork-local |
| [ADR-0673](0673-saliency-materializer-batch-manifest.md) | &lt;fill in title&gt; | Proposed | &lt;fill in&gt; |
| [ADR-0674](0674-second-opinion-materializer-batch-manifest.md) | Second-Opinion Materializer Batch Manifest | Accepted | ai, second-opinion, materializer, provenance, fork-local |
| [ADR-0675](0675-mos-label-materializer-batch-manifest.md) | MOS Label Materializer Batch Manifest | Accepted | ai, mos, materializer, provenance, fork-local |
| [ADR-0676](0676-mos-corpus-adapter-manifests.md) | MOS Corpus Adapter Manifests | Accepted | ai, mos, corpus, provenance, fork-local |
| [ADR-0677](0677-ai-dataset-fetch-manifests.md) | AI Dataset Fetch Manifests | Accepted | ai, datasets, provenance, training, fork-local |
| [ADR-0678](0678-ai-run-manifest-helper.md) | Shared AI Run Manifest Helper | Accepted | ai, provenance, docs, agents |
| [ADR-0679](0679-ci-draft-automerge-gate.md) | CI Draft Auto-Merge Gate | Accepted | ci, github-actions, merge-train, adr, fork-local |
| [ADR-0680](0680-ai-cli-helper-pattern.md) | Shared AI CLI Helper Pattern | Accepted | ai, cli, provenance, agents |
| [ADR-0681](0681-ai-script-bootstrap-helper.md) | AI Script Bootstrap Helper | Accepted | ai, cli, provenance, agents |
| [ADR-0682](0682-tiny-ai-netflix-training-scaffold-2026-05-22.md) | Tiny-AI Netflix corpus training scaffold — 2026-05-22 prep scope | Accepted | `ai`, `training`, `fork-local`, `onnx`, `mcp`, `docs` |
| [ADR-0683](0683-cjson-banned-function-remediation.md) | Replace banned functions in vendored MCP cJSON | Accepted | mcp, vendored, security, c, libvmaf, fork-local |
| [ADR-0684](0684-pre-rebase-worktree-drift-guard.md) | Pre-rebase worktree-drift guard | Accepted | agents, ci, git-hooks, fork-local |
| [ADR-0685](0685-tiny-netflix-training-scaffold-2026-05-27.md) | Tiny-AI Netflix corpus training scaffold — 2026-05-27 prep scope | Accepted | `ai`, `training`, `fork-local`, `onnx`, `mcp`, `docs` |
| [ADR-0686](0686-vmafx-rebrand-aggressive-modernization.md) | VMAFX Rebrand and Aggressive Modernization — Umbrella ADR | Proposed | rebrand, fork-policy, modernization, license, vmafx, build, ci, cli, docs |
| [ADR-0687](0687-chug-hdr-held-out-test-validator.md) | CHUG HDR MOS head — held-out test partition validator | Accepted | ai, chug, mos-head, validation, fork-local |
| [ADR-0688](0688-hip-wave32-vif-motion-fix.md) | HIP wave32 carry-preserving int64 reduction for VIF and motion kernels | Accepted | hip, numerics, vif, motion, bugfix, fork-local |
| [ADR-0689](0689-vmafx-ci-matrix-dedupe.md) | VMAFX CI Matrix Deduplication | Superseded by [ADR-1259](1259-ci-build-matrix-as-it-runs.md) | `ci`, `build`, `vmafx` |
| [ADR-0690](0690-vmafx-binary-and-ai-aliases.md) | VMAFX Binary and AI Tool Aliases | Accepted | vmafx, rebrand, cli, build, ai, mcp |
| [ADR-0691](0691-vmafx-drop-legacy-build-paths.md) | VMAFX Phase 1C — Drop Legacy Build Paths | Superseded by [ADR-1259](1259-ci-build-matrix-as-it-runs.md) | `ci`, `build`, `vmafx` |
| [ADR-0692](0692-vmafx-c23-bump.md) | Bump C standard to C23 (VMAFX rebrand Phase 1D) | Accepted | build, c, standards, meson, fork-local, vmafx-rebrand |
| [ADR-0694](0694-vmafx-lint-sanitizer-gates.md) | Tighten clang-tidy enforcement + confirm sanitizers as required CI gates | Accepted | `ci`, `lint`, `testing`, `security`, `fork-local` |
| [ADR-0696](0696-vmafx-netflix-compat.md) | `--netflix-compat` flag for restoring legacy defaults | Accepted | `vmafx`, `rebrand`, `cli`, `netflix-compat`, `fork-local` |
| [ADR-0698](0698-vmafx-production-dockerfile.md) | VMAFX Production Dockerfile — Multi-Arch, Image Signing, SBOM | Proposed | `docker`, `ci`, `release`, `security`, `sbom`, `signing`, `vmafx`, `fork-local` |
| [ADR-0699](0699-vmafx-helm-chart-k8s.md) | VMAFX Helm Chart and Kubernetes Manifests with 3-Vendor GPU Device-Plugin Support | Proposed | deploy, kubernetes, helm, gpu, cuda, hip, sycl, vulkan, fork-local |
| [ADR-0700](0700-vmafx-repo-layout.md) | VMAFX Repo Layout | Accepted | `build`, `workspace`, `meta`, `vmafx` |
| [ADR-0701](0701-vmafx-cloud-native-redesign.md) | vmafx-server HTTP transport + observability foundation | Proposed | `mcp`, `server`, `http`, `observability`, `cloud-native`, `k8s`, `vmafx` |
| [ADR-0702](0702-vmafx-phase4-language-modernization.md) | VMAFX Phase 4 — Multi-Language Modernization Foundation | Proposed | go, rust, cpp23, language-policy, modernization, tooling, fork-local, phase4 |
| [ADR-0703](0703-vmafx-server-go-grpc.md) | vmafx-server in Go — gRPC + HTTP, observability | Proposed | `server`, `go`, `grpc`, `http`, `observability`, `cloud-native`, `vmafx` |
| [ADR-0704](0704-vmafx-mcp-go-port.md) | vmafx-mcp Go port (JSON-RPC, stdio transport) | Accepted | `mcp`, `go`, `build`, `agents` |
| [ADR-0705](0705-vmafx-tune-go-stage1.md) | vmafx-tune Go port — Stage 1 (compare subcommand) | Accepted | `go`, `vmafx-tune`, `language-modernization`, `cli`, `phase4`, `fork-local` |
| [ADR-0706](0706-vmafx-rust-sys-bindings.md) | Rust `vmafx-sys` FFI bindings crate | Accepted | `rust`, `bindings`, `ffi`, `build` |
| [ADR-0707](0707-vmafx-rust-pilot-feature.md) | TAD — Temporal Absolute Difference Feature Extractor Implemented in Rust (cbindgen Pilot) | Accepted | `rust`, `build`, `metrics`, `feature-extractor`, `phase4`, `cbindgen`, `fork-local` |
| [ADR-0708](0708-vmafx-cpp23-internals-pilot.md) | C++23 Internals Pilot — `metadata_handler.c` conversion | Accepted | build, c++, cpp23, refactor, internals, fork-local, vmafx-rebrand |
| [ADR-0709](0709-vmafx-phase4b-distributed-platform.md) | VMAFX Phase 4b — Distributed Video-Quality, Encoding, and ML Platform | Proposed | `architecture`, `go`, `k8s`, `operator`, `controller`, `node`, `ffmpeg`, `rclone`, `ebpf`, `onnx`, `training`, `abi`, `platform`, `phase4b`, `fork-local` |
| [ADR-0710](0710-vmafx-ci-slim-down-v2.md) | VMAFX CI Slim-Down v2 — One Build per OS + State-of-the-Art Sanitizers | Superseded by [ADR-1259](1259-ci-build-matrix-as-it-runs.md) | `ci`, `build`, `sanitizers`, `vmafx` |
| [ADR-0711](0711-vmafx-controller-impl.md) | vmafx-controller Phase 4b.1 — Job Queue, Node Registry, and Scheduler | Accepted | `architecture`, `go`, `controller`, `grpc`, `sqlite`, `job-queue`, `node-registry`, `scheduler`, `phase4b`, `fork-local` |
| [ADR-0712](0712-ide-config-multilang-refresh.md) | IDE config audit and refresh for multi-language post-rebrand VMAFX | Accepted | docs, ide, go, rust, build, phase4, vmafx |
| [ADR-0713](0713-vmafx-node-impl.md) | vmafx-node Go Worker Binary | Proposed | `go`, `node`, `grpc`, `libvmaf`, `cgo`, `onnx`, `ffmpeg`, `k8s`, `phase4b`, `fork-local` |
| [ADR-0714](0714-vmafx-operator-skeleton.md) | vmafx-operator kubebuilder skeleton + CRDs | Accepted | `go`, `k8s`, `operator`, `crd`, `controller-runtime`, `phase4b`, `fork-local` |
| [ADR-0717](0717-vmafx-node-ffmpeg-latest.md) | vmafx-node — ffmpeg latest-tag pinning + ffmpeg-patches bundled into Dockerfile | Accepted | node, ffmpeg, docker, phase4b, fork-local |
| [ADR-0761](0761-cpp23-wave8-opt-read-json-model.md) | C++23 Wave 8 — opt.cpp activation + read_json_model.cpp | Accepted | `build`, `c++`, `cpp23`, `refactor`, `internals`, `fork-local` |
| [ADR-0763](0763-cuda-adm-decouple-ldg.md) | CUDA `adm_decouple` kernels: `__ldg()` F3 fix | Accepted | cuda, performance, adm, fork-local |
| [ADR-0764](0764-psnr-hvs-ldg-launch-bounds.md) | psnr_hvs CUDA kernel — `__ldg()` + `__restrict__` + `__launch_bounds__(64)` | Accepted | `cuda`, `perf`, `psnr_hvs`, `fork-local` |
| [ADR-0767](0767-phase-4b8-c-abi-break-scoping.md) | Phase 4b.8 — libvmaf C ABI Break for VMAFx v4.0.0 | Proposed | `api`, `abi`, `phase4b`, `breaking-change`, `v4`, `ffmpeg-patches`, `fork-local` |
| [ADR-0768](0768-cpp23-wave9-pool-env.md) | C++23 Wave 9 — picture_pool + gpu_picture_pool | Accepted | `build`, `c++`, `cpp23`, `refactor`, `internals`, `fork-local`, `vmafx-rebrand` |
| [ADR-0770](0770-vmafx-tune-go-stage4-report.md) | vmafx-tune Go port — Stage 4 (`report` subcommand) | Accepted | `go`, `vmafx-tune`, `language-modernization`, `cli`, `phase4`, `fork-local` |
| [ADR-0771](0771-simd-twin-inventory.md) | SIMD twin coverage inventory and gap prioritisation | Accepted | `simd`, `docs`, `planning` |
| [ADR-0772](0772-feature-extractor-cpp-rename.md) | Rename `feature_extractor.c` to `feature_extractor.cpp` | Accepted | `cpp23`, `build`, `core`, `fork-local` |
| [ADR-0773](0773-cuda-adm-decouple-inline-ldg.md) | CUDA ADM decouple-inline — `__ldg()` F3 fix on active path | Accepted | `cuda`, `performance`, `adm`, `fork-local` |
| [ADR-0774](0774-mcp-server-audit.md) | MCP server audit — path rename, subsample drop, schema drift, dead code | Accepted | mcp, server, audit, fork-local |
| [ADR-0777](0777-thread-safety-audit-gpu-backends.md) | Thread-Safety Audit — CUDA / SYCL / HIP Backends | Accepted | cuda, sycl, hip, thread-safety, audit, research, fork-local |
| [ADR-0778](0778-picture-pool-framesync-audit.md) | Picture pool / framesync lifecycle audit and targeted fixes | Accepted | `correctness`, `picture-pool`, `framesync`, `refcount`, |
| [ADR-0779](0779-ebpf-fuse-bypass.md) | eBPF FUSE read-path bypass for vmafx-node rclone mounts | Proposed | `ebpf`, `node`, `rclone`, `performance`, `phase4b`, `fork-local` |
| [ADR-0780](0780-nolint-cluster-refactor.md) | NOLINT Cluster Refactor — Slab Allocator, SYCL Stride, ADM Band-Size | Proposed | `ci`, `simd`, `cuda`, `sycl`, `hip`, `lint` |
| [ADR-0781](0781-sidecar-sgd-ema-online-trainer.md) | Sidecar online training — SGD + EMA + replay buffer | Proposed | ai, sidecar, online-learning, k8s, vmafx-node, phase4b, fork-local |
| [ADR-0782](0782-otel-tracing.md) | OpenTelemetry tracing and metrics schema for the VMAFX platform | Accepted | `observability`, `go`, `platform`, `adr-0782` |
| [ADR-0783](0783-k8s-e2e-integration-test-harness.md) | Kubernetes end-to-end integration test harness — kind + kuttl | Proposed | `ci`, `testing`, `k8s`, `github` |
| [ADR-0784](0784-integer-ssim-avx2.md) | AVX2 SIMD path for integer SSIM horizontal moment accumulation | Accepted | `simd`, `x86`, `avx2`, `ssim`, `performance`, `fork-local` |
| [ADR-0786](0786-vmafx-operator-stage2-reconcilers.md) | vmafx-operator Stage 2 — reconciler loops, webhook validation, per-controller RBAC | Accepted | `go`, `k8s`, `operator`, `crd`, `controller-runtime`, `phase4b`, `fork-local` |
| [ADR-0787](0787-libvmaf-api-error-path-audit.md) | libvmaf Public API Error-Path Consistency Audit | Accepted | api, error-handling, cuda, sycl, hip, consistency |
| [ADR-0788](0788-doxygen-thread-safety-tags.md) | Doxygen doc-comment and @thread-safety tags on public C-API | Accepted | api, docs, thread-safety, libvmaf |
| [ADR-0790](0790-containerfile-layer-optimization.md) | Containerfile layer optimization — merge apt layer, strip build artifacts, no-cache-dir pip | Accepted | `build`, `docker`, `containerfile`, `fork-local` |
| [ADR-0793](0793-nightly-workflow-audit.md) | Nightly Workflow Audit — TSan, Artifact Retention, Python Version | Accepted | `ci`, `nightly`, `sanitizers`, `artifacts`, `fork-local` |
| [ADR-0794](0794-controller-multi-tenant-auth-gateway.md) | Multi-Tenant Auth Gateway for vmafx-controller | Accepted | `security`, `controller`, `auth`, `multi-tenant`, `oidc`, `grpc` |
| [ADR-0797](0797-openapi-rest-schema.md) | vmafx-server OpenAPI REST contract | Accepted | server, api, rest, openapi, swagger, go, vmafx-server |
| [ADR-0802](0802-ci-runner-image-standardization.md) | CI Runner Image Standardization — Pin ubuntu-latest to ubuntu-24.04 | Accepted | `ci`, `build` |
| [ADR-0804](0804-vmaf-context-get-backend.md) | Add `vmaf_context_get_backend` — additive ABI introspection | Accepted | api, abi, backend, gpu, fork-local |
| [ADR-0806](0806-feature-dictionary-ownership.md) | VmafFeatureDictionary caller-ownership contract | Superseded by [ADR-1166](1166-upstream-issue-harvest.md) | `api`, `memory`, `testing` |
| [ADR-0809](0809-cli-cpp23-conversion.md) | C++23 Wave 8 — CLI conversion (`cli_parse.c` → `.cpp`, `vmaf.c` → `.cpp`) | Accepted | `cpp23`, `build`, `cli`, `raii`, `fork-local` |
| [ADR-0811](0811-security-codeql-go-pvr.md) | Security hardening — CodeQL Go coverage + codeql-config | Accepted | `ci`, `security`, `codeql`, `go`, `dependabot`, `ossf` |
| [ADR-0812](0812-renovate-go-rust-scheduling.md) | Renovate — Go/Cargo grouping, schedule, and concurrent-PR cap | Accepted | `ci`, `build`, `deps` |
| [ADR-0815](0815-operator-node-distroless-dockerfiles.md) | Distroless Dockerfiles for vmafx-operator and vmafx-node | Accepted | `docker`, `ci`, `release`, `operator`, `node`, `k8s`, `phase4b`, `fork-local` |
| [ADR-0819](0819-dev-container-ci-gate.md) | PR-time CI gate for dev/Containerfile | Accepted | `ci`, `build`, `workspace` |
| [ADR-0844](0844-float-adm-avx2-512-f2-f3.md) | float_adm AVX2/AVX-512 F2+F3 — double-precision and FP-contraction | Accepted | simd, bit-exactness, avx2, avx512, float_adm, build |
| [ADR-0845](0845-cuda-motion-launch-overhead.md) | CUDA motion — multi-frame SAD batching to reduce per-launch overhead | Proposed | `cuda`, `performance`, `motion`, `fork-local` |
| [ADR-0848](0848-per-surface-doc-compliance-audit.md) | Per-Surface Documentation Compliance Audit — Session 2026-05-29 | Accepted | `docs`, `compliance`, `process`, `per-surface-bar`, `fork-local` |
| [ADR-0852](0852-hip-speed-extractor-wiring.md) | Wire speed_chroma_hip and speed_temporal_hip into HIP Build and Dispatch | Accepted | `hip`, `build`, `speed`, `feature`, `gpu`, `fork-local` |
| [ADR-0854](0854-motion-avx512-parity-tests.md) | Direct AVX-512 parity tests for motion kernels | Accepted | not recorded |
| [ADR-0858](0858-cpp23-gpu-dispatch-env.md) | C++23 conversion of `gpu_dispatch_env.c` | Accepted | build, c++, cpp23, refactor, internals, fork-local, gpu-dispatch |
| [ADR-0866](0866-wire-markdownlint-into-lint-pipeline.md) | Wire markdownlint-cli2 into make lint + pre-commit + CI | Accepted | ci, docs, lint, hygiene |
| [ADR-0873](0873-arm64-neon-bit-exactness-audit.md) | ARM64 NEON bit-exactness audit — `-ffp-contract=off` carve-out scope | Accepted | `simd`, `arm64`, `neon`, `bit-exactness`, `build`, `ci` |
| [ADR-0891](0891-simd-bit-exact-round2-fmaf-libvmaf-feature-icx.md) | SIMD bit-exactness round-2 — unify SSIMULACRA 2 colour-matrix on FMA, extend `-fp-model=precise` to `libvmaf_feature_static_lib` | Accepted | `simd`, `build`, `bit-exact`, `icx` |
| [ADR-0899](0899-bash-strict-mode-sweep.md) | Bash strict-mode + trap-cleanup sweep across in-tree shell scripts | Accepted | `ci`, `agents`, `shell`, `hygiene` |
| [ADR-0904](0904-cargo-machete-ignore-build-deps.md) | Pin `cargo-machete` ignore entries for `bindgen` / `cbindgen` build dependencies | Accepted | rust, build, ci, workspace |
| [ADR-0908](0908-slow-test-audit-2026-05-30.md) | Slow-test audit (2026-05-30) — no &gt;30 s tests found; install `slow` marker as a future gate | Accepted | ci, testing, devx |
| [ADR-0915](0915-clang-tidy-modernize-sweep.md) | Enable clang-tidy `modernize-*` family with curated opt-outs | Accepted | `lint`, `ci`, `cpp`, `quality-gate` |
| [ADR-0860](0860-ffmpeg-patch-chain-no-op-vulkan-shim.md) | Re-include Vulkan FFmpeg patches as no-op shims for chain coherence | Date       \| Supersedes \| Superseded by | ci, ffmpeg, patches, fork-local |
| [ADR-0882](0882-fuzz-target-audit-json-model-sidecar.md) | Fuzz target audit — JSON model + DNN sidecar harness expansion | Accepted | ci, security, fuzzing, dnn |
| [ADR-0861](0861-vmafx-copyright-policy-drop-anthropic.md) | Drop "and Claude (Anthropic)" from fork copyright lines | Accepted | license, docs, governance |
| [ADR-0892](0892-conventional-commits-and-changelog-fragment-hygiene.md) | Conventional-Commits coverage + Changelog-fragment section hygiene | Accepted | process, release, changelog, ci, fork-local |
| [ADR-0865](0865-ansnr-sunset-pre-vmaf-metric-drop.md) | Sunset ANSNR — drop `ansnr` / `float_ansnr` feature extractors | Accepted | `metric`, `feature-extractor`, `breaking-change`, `cleanup`, `fork-local` |
| [ADR-0871](0871-ssim-dispatch-pthread-once.md) | SSIM SIMD dispatch installation must be pthread_once-guarded | Accepted | simd, threading, correctness, ssim, tsan |
| [ADR-0869](0869-sanitizer-pass-cleanup.md) | Sanitizer-Pass Cleanup — CAMBI Option-Type Mismatch and AVX{2,512} ADM Signed-Shift UB | Accepted | `c`, `simd`, `sanitizer`, `correctness`, `cambi`, `adm` |
| [ADR-0887](0887-vmaf-model-slopes-feature-mismatch-validation.md) | Reject JSON models whose per-feature arrays disagree on length | Accepted | security, parser, model, fuzz, hardening |
| [ADR-0930](0930-helm-networkpolicy-pss.md) | Ship NetworkPolicy default-deny + Pod Security Standards "restricted" in the VMAFX Helm chart | Accepted | helm, kubernetes, security, networkpolicy, podsecurity, fork-local |
| [ADR-0927](0927-opentelemetry-traces-metrics-phase1.md) | OpenTelemetry traces + metrics — Phase 1 pilot in vmafx-controller | Accepted | observability, otel, go, controller, vmafx-rebrand, phase4b, modernization |
| [ADR-0925](0925-go-generic-registry.md) | Generic in-memory registry for vmafx-controller subsystems | Accepted | `go`, `controller`, `refactoring`, `observability` |
| [ADR-0928](0928-vmaf-picture-v2-explicit-backend-state.md) | VmafPicture v2 — explicit per-backend GPU state | Proposed | api, abi, gpu, cuda, sycl, hip, metal, ffmpeg, rust, fork-local, vmafx-rebrand |
| [ADR-0926](0926-parquet-schema-v2.md) | Parquet schema v2 — canonical column order, zstd-3, schema metadata | Accepted | ai, data, storage, parquet, k150k, chug |
| [ADR-0924](0924-native-pre-commit-hooks.md) | Native bash pre-commit hook as opt-in alternative to the pre-commit framework | Accepted | build, ci, dx, tooling, fork-local, vmafx-modernization |
| [ADR-0923](0923-buildkit-cache-mounts.md) | Adopt BuildKit cache mounts and ccache across the container build matrix | Accepted | ci, build, container, performance |
| [ADR-0931](0931-mcp-cgo-direct-replace-subprocess.md) | MCP server — replace subprocess delegation with direct cgo (Phase 1) | Proposed | `mcp`, `go`, `cgo`, `libvmaf`, `performance`, `vmafx`, `modernization` |
| [ADR-0929](0929-rust-safe-binding-scaffold.md) | Rust `vmafx` safe binding crate — Phase 1 scaffold | Accepted | `rust`, `bindings`, `ffi`, `phase4`, `workspace`, `fork-local` |
| [ADR-0953](0953-doxygen-public-api-clean.md) | Doxygen public-API build is warning-clean | Accepted | `docs`, `ci`, `api`, `public-surface` |
| [ADR-0959](0959-metal-kernel-coverage-round4-closeout.md) | Metal kernel parity coverage round 4 — closeout | Accepted | testing, metal, gpu, parity, regression-guard |
| [ADR-0960](0960-gpu-runtime-error-path-leaks-round25.md) | GPU runtime error-path leak fixes — round 25 (A.1 + A.2 + A.3) | Accepted | `cuda`, `memory`, `threading`, `correctness`, `fork-local` |
| [ADR-0961](0961-queue-pullwork-rollback-on-get-failure.md) | Controller queue — roll back PullWork on post-update Get failure (round-25 audit B.1) | Accepted | go, controller, correctness, queue, phase4b, fork-local |
| [ADR-0963](0963-ai-nan-propagation-guards-round25.md) | ai/src: guard NaN propagation in eval + tune (round-25 audit C.1 + C.2) | Accepted | `ai`, `correctness`, `bisect` |
| [ADR-0966](0966-dev-containerfile-libvmaf-rename.md) | Fix dev/Containerfile post-ADR-0700 libvmaf → core paths (Round 26 audit C.1) | Accepted | `dev`, `docker`, `containerfile`, `rename`, `adr-0700`, `fork-local` |
| [ADR-0968](0968-ci-scripts-rebrand-proof-and-tempfile-trap.md) | CI scripts — rebrand-proof assertion-density grep + tempfile EXIT trap in changelog concat (Round 26 audit D.1 + D.2) | Accepted | `ci`, `build`, `docs` |
| [ADR-0969](0969-helm-seccomp-default-plus-node-image-helper.md) | Helm chart — add seccompProfile default and fix node-deployment image helper (Round 26 audit B.1 + B.3) | Accepted | `security`, `helm`, `kubernetes` |
| [ADR-0967](0967-mcp-http-transport-security-hardening.md) | MCP HTTP transport security — add auth + body limit + safer bind default (Round 26 audit A.1) | Accepted | security, mcp, http, auth, hardening, fork-local |
| [ADR-0922](0922-coverage-ratchet-aggressive.md) | Aggressive coverage ratchet + per-PR coverage-delta gate | Accepted | ci, coverage, gate, fork-local |
| [ADR-0958](0958-hip-kernel-coverage-round4.md) | HIP kernel parity-test coverage round 4 | Accepted | `hip`, `tests`, `gpu`, `coverage`, `fork-local` |
| [ADR-0962](0962-controller-streamjobs-and-reaper-stop.md) | Controller fixes — implement StreamJobs snapshot and add reaper stop signal (round-25 audit B.3 + B.4) | Accepted | controller, grpc, go, correctness, goroutine, phase4b, fork-local |
| [ADR-0956](0956-cuda-kernel-coverage-round4.md) | CUDA kernel parity coverage — round 4 (last 5 uncovered kernels) | Accepted | testing, cuda, parity, fork-local, gpu-coverage |
| [ADR-0964](0964-implement-speed-internal-and-wire-gpu-speed-extractors.md) | Implement `speed_internal.c` and wire `speed_{chroma,temporal}_{hip,sycl}` | Accepted | `cuda`, `hip`, `sycl`, `feature-extractor`, `cross-backend-parity`, `speed` |
| [ADR-0965](0965-cuda-speed-tu-repair.md) | CUDA SpEED TU repair — align with current CudaFunctions table (closes T-CUDA-SPEED-TU-REPAIR-2026-05-31) | Accepted | `cuda`, `feature-extractor`, `cross-backend-parity`, `speed`, `fork-local` |
| [ADR-0937](0937-mkdocs-nav-decade-buckets.md) | mkdocs ADR nav — per-hundred bucket layout + auto by-tag indexes | Accepted | docs, mkdocs, adr, navigation, automation, fork-local |
| [ADR-0933](0933-grpc-streaming-multi-frame-scoring.md) | gRPC streaming for multi-frame scoring (`ScoreStream`) | Accepted | grpc, server, api, streaming, fork-local |
| [ADR-0938](0938-feature-extractor-coverage-round2.md) | Feature-extractor coverage round 2 — seven CPU-side test executables | Accepted | tests, coverage, ci |
| [ADR-0935](0935-go-errors-join-slog-audit.md) | Wrap multi-step Go cleanup paths with `errors.Join`; standardise `slog` error keys | Accepted | `go`, `observability`, `refactor`, `phase4b` |
| [ADR-0936](0936-pathlib-sweep-final.md) | Final `os.path` → `pathlib.Path` sweep + ruff PTH guard | Accepted | python, lint, modernization, build |
| [ADR-0932](0932-iter-seq-adapter.md) | `iter.Seq[T]` companion APIs for single-pass Go collections | Accepted | `go`, `api`, `performance`, `ergonomics` |
| [ADR-0939](0939-skills-library-expansion.md) | Skills library expansion — MCP, k8s, audit, bisect consolidation | Accepted | agents, skills, mcp, k8s, audit, fork-local |
| [ADR-0934](0934-dataclass-to-pydantic-configs.md) | Migrate user-input dataclass configs to pydantic v2 BaseModel | Accepted | ai, validation, configs, modernization |
| [ADR-0955](0955-compat-python-vmaf-scanf-locale-bugs.md) | Fix two latent bugs in upstream-mirror `compat/python-vmaf/` (scanf width handling + ProcessRunner locale forcing) | Accepted | python-harness, upstream-mirror, fix, locale, scanf |
| [ADR-0954](0954-gpu-runtime-coverage-test.md) | Host-only unit test for shared GPU dispatch runtime | Accepted | `test`, `gpu`, `cuda`, `hip`, `sycl`, `runtime` |
| [ADR-0952](0952-test-vendored-libsvm-iqa-coverage.md) | Push test coverage on vendored libsvm + IQA paths the fork uses | Accepted | test, coverage, vendored, security, libsvm, iqa |
| [ADR-0950](0950-hip-adm-parity-feature-name-and-enosys-skip.md) | Fix symmetric "adm" vs "adm_hip" feature-name bug in test_hip_adm_parity and add ENOSYS skip | Accepted | test, hip, parity, fork-local |
| [ADR-0949](0949-hip-motion3-parity-enosys-skip.md) | HIP motion3 parity test skips cleanly when HIPCC kernels are not built | Accepted | hip, tests, bugfix, fork-local, dx |
| [ADR-0951](0951-github-actions-custom-audit.md) | GitHub Actions custom-action and reusable-workflow audit | Accepted | `ci`, `docs`, `process` |
| [ADR-0947](0947-cuda-kernel-coverage-round3.md) | CUDA kernel parity coverage — round 3 (float-path twins + ssimulacra2) | Accepted | testing, cuda, parity, fork-local, gpu-coverage |
| [ADR-0948](0948-feature-extractor-coverage-round3.md) | Feature-extractor coverage round 3 — targeted unit tests for low-coverage files | Accepted | `test`, `coverage`, `feature` |
| [ADR-0945](0945-hip-kernel-coverage-round3.md) | HIP kernel parity-test coverage round 3 | Accepted | `hip`, `tests`, `gpu`, `coverage`, `fork-local` |
| [ADR-0914](0914-unified-python-test-orchestrator.md) | Unified Python test orchestrator (nox at repo root) | Accepted | `build`, `ci`, `python`, `ai`, `mcp`, `tools` |
| [ADR-0917](0917-cargo-deny-supply-chain-policy.md) | cargo-deny supply-chain policy enforcement | Accepted | `security`, `ci`, `rust`, `supply-chain`, `license` |
| [ADR-0918](0918-llvm-ir-diff-harness.md) | LLVM IR diff harness for bit-exact SIMD paths | Accepted | simd, build, ci, perf, diagnostics, fork-local |
| [ADR-0912](0912-pixel-format-edge-coverage.md) | Pixel-format edge coverage at the libvmaf unit-test layer | Accepted | test, coverage, fork-local, pixel-format, hbd |
| [ADR-0913](0913-changelog-renderer-splice-contract.md) | Changelog renderer uses bracketed H2 headings as splice boundaries | Accepted | `release`, `docs`, `tooling` |
| [ADR-0719](0719-vmafx-node-rclone-integration.md) | vmafx-node rclone Integration — Remote-Asset Streaming Without Disk Materialisation | Accepted | `architecture`, `go`, `node`, `rclone`, `storage`, `ffmpeg`, `phase4b`, `fork-local` |
| [ADR-0720](0720-cpp23-pilot-mem.md) | C++23 Wave-1 Pilot — `mem.c` conversion | Accepted | build, c++, cpp23, refactor, internals, fork-local, vmafx-rebrand |
| [ADR-0721](0721-cpp23-pilot-opt.md) | C++23 Pilot Wave 1 — `opt.c` conversion | Accepted | build, c++, cpp23, refactor, internals, fork-local, vmafx-rebrand |
| [ADR-0723](0723-cpp23-pilot-fex-ctx-vector.md) | C++23 Pilot — `fex_ctx_vector.c` Conversion (Wave 2) | Accepted | build, c++, cpp23, refactor, internals, fork-local, vmafx-rebrand |
| [ADR-0725](0725-cpp23-pilot-log-v2.md) | C++23 Pilot — `log.c` conversion (real C++23, supersedes ADR-0722) | Accepted | build, c++, cpp23, refactor, internals, fork-local, vmafx-rebrand |
| [ADR-0726](0726-drop-vulkan-backend.md) | Drop Vulkan backend | Accepted | vulkan, gpu, backend, build, breaking, fork-local |
| [ADR-0727](0727-cpp23-wave2-bump-and-dict.md) | C++23 Wave 2 — project-wide `cpp_std=c++23` bump and `dict.c` → `dict.cpp` | Accepted | `build`, `c++`, `cpp23`, `refactor`, `internals`, `fork-local`, `vmafx-rebrand` |
| [ADR-0728](0728-native-build-sunset.md) | Sunset Legacy Native Build Modes — Phase 4b.9 Follow-On | Superseded by [ADR-1259](1259-ci-build-matrix-as-it-runs.md) | `ci`, `build`, `vmafx`, `breaking` |
| [ADR-0729](0729-cpp23-wave3-bundle.md) | C++23 Wave 3 — feature_name, picture_copy, model | Accepted | `build`, `cpp23`, `refactor` |
| [ADR-0730](0730-vmafx-tune-go-stage2.md) | vmafx-tune Go port — Stage 2 (ladder subcommand) | Accepted | `go`, `vmafx-tune`, `language-modernization`, `cli`, `phase4`, `fork-local` |
| [ADR-0731](0731-cpp23-wave3-part-b.md) | C++23 Wave 3 Part B — psnr_tools, luminance_tools, mkdirp | Accepted | `build`, `cpp23`, `modernization` |
| [ADR-0733](0733-cpp23-wave4-output-writers.md) | C++23 Wave 4 — output writers (XML, JSON, CSV, subtitle) | Accepted | `build`, `c++`, `cpp23`, `refactor`, `internals`, `fork-local`, `vmafx-rebrand` |
| [ADR-0735](0735-cpp23-wave5-bundle.md) | C++23 Wave 5 — cpu, ref, thread_locale | Accepted | `build`, `c++`, `cpp23`, `refactor`, `internals`, `fork-local` |
| [ADR-0738](0738-bump-cuda-133-r610-local.md) | Bump local CUDA toolkit pin to 13.3 + R610 minimum driver (partial — CI deferred) | Accepted | `cuda`, `build`, `container`, `ci`, `deps` |
| [ADR-0743](0743-cuda-vif-filter1d-ncu-driven-perf.md) | CUDA VIF filter1d ncu-driven performance optimizations | Accepted | `cuda`, `performance`, `vif` |
| [ADR-0744](0744-cuda-ms-ssim-adm-cm-ncu-driven-perf.md) | CUDA adm_cm `__launch_bounds__(128, 8)` register reduction (ms_ssim_decimate smem tiling reverted) | Accepted | cuda, performance, adm, ms_ssim, occupancy |
| [ADR-0746](0746-cuda-integer-adm3-aim-parity.md) | integer_adm_cuda — emit integer_adm3 + integer_aim (parity with CPU) | Accepted | `cuda`, `integer_adm`, `aim`, `adm3`, `parity` |
| [ADR-0747](0747-cuda-extern-c-sweep.md) | CUDA `extern "C"` invariant for host-looked-up kernels | Accepted | not recorded |
| [ADR-0749](0749-sunset-legacy-vmaf-feature-extractor.md) | Sunset VmafLegacyQualityRunner (float-path runner) | Accepted | `python`, `quality-runner`, `breaking-change`, `cleanup` |
| [ADR-0750](0750-cuda-ms-ssim-decimate-adm-cm-measure.md) | Hardware Measurement Verdict for PR perf/cuda-ms-ssim-decimate-adm-cm-ncu-driven | Accepted | cuda, performance, ms_ssim, adm_cm, measurement |
| [ADR-0752](0752-perf-bench-multi-resolution.md) | Multi-Resolution Performance Benchmark Baseline | Accepted | not recorded |
| [ADR-0753](0753-cuda-resolution-aware-dispatch.md) | Runtime Resolution-Aware CUDA Kernel Variant Dispatch | Accepted | `cuda`, `perf`, `build` |
| [ADR-0754](0754-cuda-ssim-vert-combine-ldg-pinned-leak.md) | CUDA SSIM `vert_combine`: `__ldg()` + `__launch_bounds__` + pinned-host leak fix | Accepted | cuda, performance, correctness, ssim, fork-local |
| [ADR-0755](0755-cpp23-wave7-single-file.md) | C++23 Wave 7 — drop orphan `cpu.c`, activate `cpu.cpp` | Accepted | `cpp23`, `build`, `core`, `fork-local` |
| [ADR-0756](0756-cuda-f3-struct-by-value-audit.md) | CUDA F3 struct-by-value kernel audit (scope + dispatch order) | Accepted | `cuda`, `perf`, `research` |
| [ADR-0757](0757-cuda-ms-ssim-vert-lcs-horiz-ldg.md) | CUDA MS-SSIM `ms_ssim_vert_lcs` + `ms_ssim_horiz`: `__ldg()` + `__launch_bounds__` (F3 fix #2) | Accepted | cuda, performance, ms_ssim, fork-local |
| [ADR-0759](0759-hip-adm-buffer-by-pointer.md) | HIP ADM — AdmBufferHip passed by pointer (F3 fix) | Accepted | `hip`, `performance`, `cuda`, `kernel`, `adm`, `fork-local` |
| [ADR-0760](0760-cuda-motion-ncu-multi-resolution.md) | CUDA motion kernel multi-resolution ncu profiling methodology | Accepted | `cuda`, `perf`, `research` |
| [ADR-0762](0762-cuda-ciede-ldg.md) | CUDA CIEDE2000 8bpc/16bpc — `__ldg()` read-only cache routing (F3 fix) | Accepted | `cuda`, `performance`, `ciede`, `fork-local` |
| [ADR-0775](0775-dnn-ort-audit.md) | DNN ORT Backend Audit Findings | Accepted | dnn, onnx, ort, thread-safety, correctness, fork-local, research |
| [ADR-0792](0792-hardcoded-yuv-path-env-overrides.md) | Env-var overrides for hardcoded YUV and testdata paths | Accepted | `workspace`, `ci`, `testdata` |
| [ADR-0795](0795-prev-ref-thread-safety.md) | Clarify and harden VmafFeatureExtractor.prev_ref thread-safety invariant | Accepted | threading, feature-extractor, batch-threading, correctness |
| [ADR-0810](0810-adr-0108-compliance-audit-2026-05-29.md) | Six-Deliverables Compliance Audit (2026-05-29) + D3 Gap Fixes | Accepted | `docs`, `agents`, `process` |
| [ADR-0839](0839-cpp23-shadow-const-fixes.md) | C++23 wave — shadow-identifier and implicit-cast cleanup | Accepted | `cpp23`, `lint`, `core`, `sycl`, `fork-local` |
| [ADR-0840](0840-gpu-dispatch-toctou-fence.md) | Fix cu_state leak on import failure and gpu_dispatch_env TOCTOU | Accepted | `cuda`, `security`, `framework`, `ci` |
| [ADR-0841](0841-env-var-consolidation.md) | Environment variable reference page and canonical naming | Accepted | `docs`, `sycl`, `cuda`, `ai`, `workspace` |
| [ADR-0853](0853-motion-avx2-remove-debug-macros.md) | Remove dead debug-print macros from motion_avx2.c | Accepted | `simd`, `lint`, `avx2`, `cleanup`, `fork-local` |
| [ADR-0911](0911-init-py-export-completeness-audit.md) | `__init__.py` export-completeness audit — `__all__` + SPDX headers across fork-added Python packages | Accepted | `docs`, `python`, `ai`, `mcp`, `tools`, `lint` |
| [ADR-0910](0910-codespell-sweep-config.md) | Project-wide codespell config + sweep policy | Accepted | docs, lint, tooling, fork-local |
| [ADR-0907](0907-perf-regression-gate-wall-clock.md) | Wall-clock perf regression gate over the multi-resolution baseline | Proposed | ci, performance, regression-gate, fork-local, testing |
| [ADR-0905](0905-gitignore-and-workflow-audit.md) | `.gitignore` + `.github/workflows/` staleness audit (2026-05-30) | Accepted | `repo-hygiene`, `ci`, `docs` |
| [ADR-0903](0903-wire-codecov-upload.md) | Wire Codecov upload into the existing Coverage Gate jobs | Accepted | ci, coverage, codecov, observability, fork-local |
| [ADR-0902](0902-signing-and-attestation-audit.md) | Signing and attestation audit — close residual gaps (2026-05-30) | Accepted | security, supply-chain, sigstore, slsa, cosign, attestation, ci, fork-local |
| [ADR-0901](0901-governance-audit.md) | Governance file audit — add GOVERNANCE + MAINTAINERS, expand CODEOWNERS, document ADR-0108 in CONTRIBUTING | Accepted | `governance`, `docs`, `meta` |
| [ADR-0893](0893-pre-commit-audit-2026-05-30.md) | Pre-commit config audit — 2026-05-30 | Accepted | ci, lint, hygiene, pre-commit |
| [ADR-0889](0889-libsvm-vendored-audit.md) | Vendored libsvm 3.24 audit — close header-row-ordering oob, document upstream-version policy | Accepted | vendored, security, libvmaf, fork-local |
| [ADR-0890](0890-ci-concurrency-cost-audit.md) | CI concurrency + cost audit follow-up to PR #301 | Accepted | ci, cost |
| [ADR-0884](0884-sycl-kernel-coverage-round2.md) | SYCL kernel coverage round 2 — five additional CPU-vs-SYCL parity gates | Accepted | testing, sycl, gpu, parity, fork-local |
| [ADR-0886](0886-cuda-kernel-coverage-round2.md) | CUDA kernel parity test coverage — round 2 gap-fill | Accepted | testing, cuda, gpu, parity, coverage |
| [ADR-0883](0883-hip-kernel-coverage-round2.md) | HIP kernel parity-test coverage round 2 | Accepted | `hip`, `tests`, `gpu`, `coverage` |
| [ADR-0880](0880-unused-testdata-debug-scripts-cleanup.md) | Remove unreferenced testdata debug scripts and orphan snapshot | Accepted | cleanup, testdata, repo-hygiene |
| [ADR-0878](0878-trivy-container-scan-baseline.md) | Trivy container scan baseline — production images run as non-root | Accepted | security, docker, ci, vmafx-rebrand, phase4b |
| [ADR-0876](0876-printf-format-portability-pri-macros.md) | Adopt `&lt;inttypes.h&gt;` PRI macros for fixed-width integer printf formatting | Accepted | portability, c-standards, cert, sycl, dnn, windows |
| [ADR-0877](0877-error-code-consistency-audit.md) | Error-code consistency audit — fork-added MS-SSIM decimate dispatcher | Accepted | c, simd, error-handling, api-contract |
| [ADR-0875](0875-github-actions-audit-2026-05-30.md) | GitHub Actions hardening audit (2026-05-30) | Accepted | `security`, `ci`, `supply-chain` |
| [ADR-0874](0874-magic-number-audit-cert-int07c.md) | Name magic numbers in fork-added C surfaces (CERT INT07-C closeout pass 1) | Accepted | cleanup, cert, mcp, dnn, picture, cuda |
| [ADR-0872](0872-io-error-and-eintr-audit.md) | POSIX I/O EINTR-retry + return-value audit on fork-added C | Accepted | `correctness`, `mcp`, `posix`, `nasa-power-of-10` |
| [ADR-0870](0870-helm-values-schema-and-container-rebuild-audit.md) | Helm chart values.schema.json + dev-MCP Containerfile rebuild audit | Accepted | helm, k8s, deploy, devx, dev-mcp, container, rebase-hygiene |
| [ADR-0868](0868-gpu-backend-kernel-coverage.md) | GPU backend kernel parity-test coverage gap-fill | Accepted | tests, cuda, hip, sycl, metal, coverage |
| [ADR-0970](0970-test-gpu-picture-pool-cleanup.md) | test_gpu_picture_pool.c: remove unused malloc + dead code (Round 27 audit D.3 + D.4) | Accepted | `testing`, `cuda`, `memory`, `cleanup`, `fork-local` |
| [ADR-0971](0971-test-unchecked-malloc-sweep.md) | Test suite: NULL-check malloc in 3 test files (Round 27 audit D.1) | Accepted | `testing`, `correctness`, `asan`, `fork-local` |
| [ADR-0972](0972-public-header-iso-reserved-guards.md) | Public headers — replace ISO-reserved `__VMAF_*__` include guards with `LIBVMAF_*_H` (Round 27 audit A.1, SEI CERT DCL37-C) | Accepted | `api`, `headers`, `cert-c`, `lint`, `compatibility` |
| [ADR-0973](0973-master-ci-regressions-verified-2026-05-31.md) | Master CI fixes — Metal MS-SSIM fixture dim + ssimulacra2 icpx XYB bit-exactness | Accepted | ci, simd, metal, ssimulacra2, icpx, bit-exactness, tests |
| [ADR-0975](0975-mcp-server-tempfile-collision.md) | Use NamedTemporaryFile in _run_vmaf_score to eliminate task-name collision risk | Accepted | `mcp`, `security`, `concurrency` |
| [ADR-0976](0976-dnn-sidecar-dead-norm-fields-removal.md) | Remove dead has_norm sidecar fields and fix extract_string_array leak | Accepted | dnn, sidecar, cleanup, leak, security |
| [ADR-0977](0977-core-tools-input-reader-safety.md) | core/tools input-reader safety — Y4M malloc-NULL check, YUV/Y4M size_t cast, bench GPU-state leaks | Accepted | `security`, `bug`, `tools`, `audit` |
| [ADR-0978](0978-vmafx-server-bug-audit.md) | vmafx-server + pkg/score bug-audit — shutdown leak, gRPC Send-EOF surfacing, HTTP body cap, panic recovery | Accepted | `security`, `bug`, `audit`, `go`, `grpc`, `http`, `server` |
| [ADR-0980](0980-markdown-lint-full-ruleset-discharge.md) | Markdown-lint full-ruleset discharge — content fixes + per-file scoped disables | Accepted | `docs`, `lint`, `ci`, `policy`, `fork-local` |
| [ADR-0982](0982-gpu-runtime-bug-audit-round-26.md) | GPU runtime bug audit — round 26 (init/teardown leak sweep) | Accepted | `cuda`, `sycl`, `gpu`, `lifecycle`, `audit` |
| [ADR-0983](0983-gosec-findings-fix-sweep.md) | gosec sweep — fix all findings + add CI gate | Accepted | security, ci, go |
| [ADR-0862](0862-k150k-crash-restart-row-loss-consistency-check.md) | K150K extractor — .done vs parquet consistency check on restart | Accepted | `ai`, `pipeline`, `durability`, `k150k` |
| [ADR-0879](0879-python-dep-freshness-2026-05-30.md) | Python dependency freshness sweep (2026-05-30) | Accepted | security, ai, mcp, deps, python, fork-local |
| [ADR-0881](0881-coverage-overrides-audit-2026-05-30.md) | Coverage-overrides audit — tighten template coverage floor | Accepted | ci, coverage, dnn, gate, audit, adr-0114 |
| [ADR-0888](0888-pyright-strict-audit.md) | Pyright strict audit of fork-local Python packages | Accepted | ai, mcp, python, type-safety, ci |
| [ADR-0946](0946-sycl-kernel-coverage-round3.md) | SYCL kernel coverage round 3 (float family + PSNR-HVS) | Accepted | sycl, test, gpu, parity, kernel-coverage |
| [ADR-0957](0957-sycl-kernel-coverage-round4.md) | SYCL kernel coverage round 4 (float_moment + SpEED + SSIMULACRA2) | Accepted | sycl, test, gpu, parity, kernel-coverage |
| [ADR-0984](0984-port-upstream-netflix-may-jun-2026.md) | Port Netflix Upstream May–Jun 2026 (5 commits) | Accepted | not recorded |
| [ADR-0985](0985-sycl-parity-divergence-2026-06-03.md) | SYCL parity divergence investigation — float_ssim + ssimulacra2 on Arc A380 | Accepted | sycl, parity, ci, gpu, precision, arc |
| [ADR-0986](0986-ci-docs-pr-trigger.md) | Add PR trigger to docs.yml CI workflow | Accepted | `ci`, `docs`, `fork-local` |
| [ADR-0987](0987-avx512-float-moment.md) | AVX-512 path for float_moment feature extractor | Accepted | `simd`, `avx512`, `performance`, `float_moment`, `fork-local` |
| [ADR-0988](0988-shared-strict-json-helpers.md) | Route strict-JSON helpers through `vmaftune.jsonio` across vmaf-tune | Accepted | `refactor`, `json`, `vmaf-tune`, `mcp` |
| [ADR-0989](0989-sycl-motion-add-uv.md) | Wire motion_add_uv through integer_motion_sycl; emit warning on motion_five_frame_window | Accepted | `sycl`, `motion`, `feature-extractor`, `gpu` |
| [ADR-0990](0990-cuda-ms-ssim-double-precision-lcs.md) | Restore double-precision L/C/S accumulation in CUDA ms_ssim_vert_lcs | Accepted | cuda, precision, ms-ssim, bit-exactness |
| [ADR-0991](0991-second-opinion-batch-runs.md) | Second-Opinion Batch Materializer — Smoke-Run Scaffold and Test Fix | Accepted | ai, second-opinion, materializer, testing, smoke, fork-local |
| [ADR-0992](0992-mos-label-batch-runs.md) | MOS-label batch-run manifests for KonViD and CHUG | Accepted | ai, mos, training, corpus, konvid, chug, fork-local |
| [ADR-0993](0993-konvid-ugc-bvi-saliency-batch-launch.md) | KoNViD / UGC / BVI-DVC Saliency Batch Manifests and Run Scaffolding | Accepted | ai, saliency, materializer, konvid, ugc, bvi-dvc, batch, fork-local |
| [ADR-0994](0994-coverage-build-fix-motion-v2-ref.md) | Fix Coverage Gate build break — `integer_motion.c` compile error | Accepted | `ci`, `coverage`, `build`, `motion` |
| [ADR-0995](0995-ci-workflow-name-shortening.md) | Shorten CI workflow and job display names | Accepted | `ci`, `github-actions`, `docs` |
| [ADR-0996](0996-ebpf-fuse-bypass-rclone.md) | eBPF FUSE bypass for rclone zero-copy path in vmafx-node | Proposed | `ci`, `go`, `ebpf`, `rclone`, `performance`, `security`, `supply-chain` |
| [ADR-0999](0999-stdatomic-cxx-header-guard.md) | Guard `&lt;stdatomic.h&gt;` includes in C++ translation units (GCC 14 + Clang-18 fix) | Accepted | `build`, `ci`, `cpp`, `atomics`, `tsan`, `fork-local` |
| [ADR-1000](1000-tech-stack-badges-go-rust-pins.md) | Tech-stack badges in README and Go/Rust version pin consistency | Accepted | `docs`, `ci`, `build`, `go`, `rust` |
| [ADR-1001](1001-sycl-cambi-parity.md) | SYCL parity round 5 — CAMBI CPU vs. SYCL parity gate | Accepted | `sycl`, `test`, `gpu`, `parity`, `kernel-coverage`, `cambi`, `fork-local` |
| [ADR-1002](1002-rust-edition-2024-bindgen-072.md) | Bump Rust workspace to edition 2024 and bindgen to 0.72 | Accepted | `rust`, `build`, `workspace` |
| [ADR-1003](1003-cpp-std-c23-bump.md) | Bump project-wide C++ standard from c++11 to c++23 | Accepted | build, c++, cpp23, meson, standards, fork-local, vmafx-rebrand |
| [ADR-1004](1004-hip-kernel-coverage-round5.md) | HIP kernel parity-test coverage round 5 | Accepted | `hip`, `tests`, `gpu`, `coverage`, `fork-local` |
| [ADR-1005](1005-perf-gate-advisory-threshold.md) | Perf Gate Advisory Mode and Baseline Refresh Documentation | Accepted | `ci`, `perf` |
| [ADR-1007](1007-c-string-numeric-ub-fixes.md) | Fix C string/numeric UB cluster — NULL strcmp, size_t underflow, signed-shift overflow, snprintf truncation | Accepted | `core`, `security`, `c`, `ub` |
| [ADR-1008](1008-c-lifecycle-test-bugs.md) | Fix C lifecycle bugs — pic_cnt double-increment, div-by-zero in pooled score, silent test failures | Accepted | `core`, `correctness`, `test`, `c` |
| [ADR-1009](1009-go-shutdown-goroutine-fixes.md) | Fix Go shutdown / goroutine correctness — WaitForShutdown unconditional block, unbounded GracefulStop | Accepted | `go`, `server`, `controller`, `shutdown`, `correctness` |
| [ADR-1010](1010-mcp-json-parse-guards.md) | MCP server JSON parse guards — vmaf output and ffprobe output | Accepted | `mcp`, `python`, `error-handling`, `correctness` |
| [ADR-1011](1011-cuda-symbol-visibility.md) | Add `static` to TU-internal CUDA helper functions — VIF, ADM, motion | Accepted | `core`, `cuda`, `correctness`, `build` |
| [ADR-1012](1012-go-queue-state-machine-guards.md) | Go queue state-machine guards — PullWork AND-status, ReportResult idempotency | Accepted | `go`, `controller`, `queue`, `correctness`, `concurrency` |
| [ADR-1014](1014-r5-prometheus-registry.md) | Prometheus registry isolation for SetControllerSources | Accepted | `security`, `observability`, `go` |
| [ADR-1017](1017-r5-go-timer-ctx-cancel.md) | Go operator controller resource-allocation fixes | Accepted | `security`, `k8s`, `go`, `operator` |
| [ADR-1018](1018-r5-grpc-hardening.md) | MCP exec.CommandContext + controller gRPC panic recovery | Accepted | `security`, `mcp`, `go`, `grpc` |
| [ADR-1020](1020-r5-memory-ordering.md) | acq_rel memory ordering on ref-count decrement + mutex-destroy-after-unlock + picture-pool unlock ordering | Accepted | `correctness`, `threading`, `core` |
| [ADR-1021](1021-session-token-const-time-compare.md) | Constant-time session-token comparison + JWT nbf-claim validation | Accepted | `security`, `auth` |
| [ADR-1022](1022-y4m-dst-buf-read-sz-overflow.md) | Cast `dst_buf_read_sz` operands to `size_t` in y4m_input to prevent signed-integer overflow | Accepted | `core`, `security`, `correctness`, `tools`, `c` |
| [ADR-1023](1023-mcp-asyncio-correctness.md) | MCP server asyncio correctness — async wrappers for blocking I/O | Accepted | `mcp`, `asyncio`, `python`, `correctness` |
| [ADR-1024](1024-r6-metric-scoring-guards.md) | R6 per-metric scoring guards — PSNR/ADM correctness fixes | Accepted | `correctness`, `psnr`, `adm`, `simd` |
| [ADR-1025](1025-r6-cuda-hip-kernel-correctness.md) | R6 CUDA/HIP kernel correctness fixes | Accepted | `correctness`, `cuda`, `hip`, `simd` |
| [ADR-1026](1026-r6-sycl-kernel-correctness.md) | R6 SYCL kernel correctness — rd-stride OOB and unchecked graph_wait | Accepted | `correctness`, `sycl` |
| [ADR-1030](1030-hip-metal-kernel-correctness.md) | HIP adm_decouple dangling body + VIF wavefront 32-bit carry + Metal motion vertical halo | Accepted | `hip`, `metal`, `correctness`, `gpu` |
| [ADR-1032](1032-vmaf-init-double-init-guard-vmaf-close-pointer-contract.md) | vmaf_init double-init guard and vmaf_close pointer-contract documentation | Accepted | `api`, `correctness`, `memory-safety` |
| [ADR-1033](1033-cpu-scoring-nan-ub-guards.md) | CPU-side scoring NaN/UB guards across PSNR/SSIM/MS-SSIM/ADM/CAMBI/MOTION | Accepted | `correctness`, `cpu`, `psnr`, `ssim`, `adm`, `cambi`, `motion` |
| [ADR-1034](1034-sycl-vif-rd-stride-motion-uv-sync.md) | Fix SYCL integer_vif rd_stride OOB on odd widths and integer_motion UV queue sync gap | Accepted | `sycl`, `correctness`, `gpu` |
| [ADR-1035](1035-ci-workflow-concurrency-timeout.md) | CI workflow concurrency guards and job timeouts | Accepted | `ci`, `security`, `supply-chain` |
| [ADR-1036](1036-licensing-spdx-svm-copyright.md) | Correct SPDX license identifiers and add missing libsvm copyright | Accepted | `license`, `security`, `supply-chain` |
| [ADR-1038](1038-mcp-cross-surface-precision-subsample-drift.md) | MCP cross-surface precision-default and probe-precision drift | Accepted | `mcp`, `correctness`, `cross-surface` |
| [ADR-1039](1039-vendored-svm-realloc-oom-safety.md) | Fix CERT MEM04-C realloc OOM safety in vendored libsvm | Accepted | `memory-safety`, `correctness`, `vendored` |
| [ADR-1040](1040-integer-ssim-moments-type-non-x86.md) | Promote `integer_ssim_moments_t` to shared header (macOS / Windows arm64 build fix) | Accepted | `build`, `simd`, `arm64`, `macos`, `windows`, `integer-ssim`, `fork-local` |
| [ADR-1041](1041-ci-fix-go-rust-red.md) | Fix CI RED — Go metal option type + Rust AVX-512 test guard | Accepted | `ci`, `build`, `go`, `rust`, `avx512`, `metal` |
| [ADR-1042](1042-containerfile-user-hardening.md) | Containerfile hardening — non-root USER + build-time DEBIAN_FRONTEND | Accepted | `containerfile`, `security`, `docker`, `ci`, `hardening` |
| [ADR-1047](1047-helm-schema-bug-fixes.md) | Helm chart schema and values.yaml correctness fixes (R9 batch) | Accepted | `helm`, `k8s`, `bug` |
| [ADR-1048](1048-vmaf-tune-duration-sentinel.md) | vmaf-tune `ladder --duration` sentinel dest mismatch fix | Accepted | `vmaf-tune`, `bug`, `cli` |
| [ADR-1049](1049-grpc-feedback-backoff.md) | Exponential backoff for vmafx-node online-feedback drainLoop | Accepted | `go`, `grpc`, `node`, `bug` |
| [ADR-1051](1051-upstream-batch-threading-picture-pool.md) | Port upstream batch-threading + picture-pool defaults (dff4082b + 46d3a154) | Accepted | `upstream-port`, `scoring`, `threading`, `correctness` |
| [ADR-1052](1052-arm-motion-v2-re-register.md) | Re-register CPU `motion_v2` extractor and fix post-flush test ordering | Accepted | `core`, `motion`, `test`, `build` |
| [ADR-1053](1053-dev-cuda-passthrough.md) | Default docker-compose runtime to nvidia and expand GPU capabilities | Accepted | `dev`, `cuda`, `docker`, `build` |
| [ADR-1056](1056-msvc-cpp-std.md) | Use /std:c++latest on MSVC instead of cpp_std=c++23 | Accepted | `build`, `ci`, `windows`, `msvc` |
| [ADR-1057](1057-revert-float-adm-simd-dispatch-neon-fma.md) | Revert float-ADM SIMD dispatch wiring (PR #685) — NEON FMA divergence unfixable in scope | Superseded by PR #1161's scoped float-parity and Darwin integer-compatibility contracts; the Darwin integer-compatibility contract is superseded by [ADR-1257](1257-retire-darwin-adm-dwt2-legacy-dispatch.md) | `simd`, `neon`, `float-adm`, `integer-adm`, `darwin`, `revert`, `correctness` |
| [ADR-1058](1058-helm-chart-security-hardening.md) | Helm chart security hardening — PDB, RBAC split, metrics NetworkPolicy, schema tightening | Accepted | `helm`, `k8s`, `rbac`, `security`, `networkpolicy` |
| [ADR-1060](1060-r10-cpp23-error-paths.md) | Round 10 C++23 wave error-path cleanup | Accepted | `cpp23`, `correctness`, `memory`, `error-handling`, `fork-local` |
| [ADR-1061](1061-vendored-cjson-pdjson-depth-overflow.md) | Fix depth-limit, integer-overflow, and banned-function bugs in vendored pdjson and cJSON | Accepted | security, vendored, mcp, c, libvmaf, fork-local |
| [ADR-1063](1063-rust-clippy-library-strictness.md) | Rust clippy strictness — scoped bindings suppression, unsafe_op lint, no panicking Default | Accepted | `rust`, `lint`, `safety`, `workspace` |
| [ADR-1064](1064-ffmpeg-patches-score-fmt.md) | Wire score_fmt option on all vmaf FFmpeg filters | Accepted | `ffmpeg`, `build`, `api` |
| [ADR-1065](1065-go-staticcheck-timer-body.md) | Go staticcheck r10 — poll-loop timer leak and missing body guards | Accepted | `go`, `security`, `correctness` |
| [ADR-1066](1066-svm-multiclass-realloc-regression.md) | Regression tests for the sequential-realloc double-free in libsvm | Accepted | `test`, `security`, `ci` |
| [ADR-1068](1068-thread-safety-gpu-dispatch-env.md) | Fix fast-path data race in gpu_dispatch_env.cpp via atomic publication flag | Accepted | `core`, `correctness`, `thread-safety`, `cpp23` |
| [ADR-1069](1069-operator-crd-status-schema-gaps.md) | Operator CRD status-schema gaps and VmafxNode LastHeartbeat ownership | Accepted | `operator`, `crd`, `k8s`, `bug` |
| [ADR-1071](1071-ms-ssim-hip-double-partials.md) | Promote HIP ms_ssim_vert_lcs to double precision (ADR-0990 parity) | Accepted | `hip`, `precision`, `ms_ssim`, `cross-backend-parity` |
| [ADR-1072](1072-prev-ref-batch-refcount-leak.md) | Fix PREV_REF refcount leak in threaded batch and serial dispatch paths | Accepted | `core`, `threading`, `memory`, `picture-pool`, `bug` |
| [ADR-1073](1073-mcp-score-at-index-eagain-guard.md) | Fix vmaf_score_at_index EAGAIN-guard misapplication for model output slots | Accepted | `mcp`, `scoring`, `correctness`, `core`, `fork-local` |
| [ADR-1074](1074-helm-values-completeness.md) | Helm chart values completeness — missing knobs and schema gaps | Accepted | `helm`, `k8s`, `bug` |
| [ADR-1075](1075-mcp-http-score-body-validation.md) | MCP HTTP transport `POST /v1/score` body-validation edge cases | Accepted | `mcp`, `security`, `correctness`, `http`, `fork-local` |
| [ADR-1077](1077-vmaftune-corner-cases-r14.md) | vmaf-tune corner cases: parse_versions + compare preset | Accepted | not recorded |
| [ADR-1078](1078-ms-ssim-option-parity.md) | ms_ssim option parity across HIP and SYCL backends | Accepted | `hip`, `sycl`, `cuda`, `ms_ssim`, `parity` |
| [ADR-1079](1079-tsan-batch-thread-safety.md) | TSan-eligible thread-safety test for threaded_extract_batch_func | Accepted | `ci`, `test`, `threading` |
| [ADR-1080](1080-ubsan-enum-invalid-value-log-opt.md) | UBSan enum-invalid-value fixes in vmaf_log and vmaf_option_set | Accepted | `ci`, `sanitizer`, `build` |
| [ADR-1081](1081-bench-correctness.md) | vmaf_bench correctness — unchecked alloc returns and wall-clock timer | Accepted | `tools`, `bench`, `correctness`, `clock` |
| [ADR-1083](1083-yuv-input-edge-cases.md) | y4m_input_fetch_frame signed-integer overflow + fread(NULL) UB fixes | Accepted | `core`, `security`, `correctness`, `tools`, `c`, `fork-local`, `bugfix` |
| [ADR-1084](1084-cross-platform-path-list-separator.md) | Use filepath.SplitList for VMAF_MCP_ALLOW path-list parsing | Accepted | `build`, `windows`, `mcp` |
| [ADR-1085](1085-mcp-streaming-backpressure-disconnect.md) | MCP streaming backpressure — kill child processes on client disconnect | Accepted | `mcp`, `security`, `go`, `python` |
| [ADR-1086](1086-ci-workflow-least-privilege-permissions.md) | CI Workflow Least-Privilege Permissions Audit | Accepted | `ci`, `security` |
| [ADR-1087](1087-coverage-pkg-storage.md) | Extend test coverage for pkg/storage and cmd/vmafx-node/bpf | Accepted | `test`, `storage`, `ebpf`, `coverage` |
| [ADR-1088](1088-r14-cli-flag-parsing.md) | CLI flag-parsing hardening — parse_unsigned overflow/negative guards and --help in cli_parse.cpp | Accepted | `cli`, `security`, `correctness` |
| [ADR-1089](1089-dnn-onnx-domain-bypass.md) | Block non-standard ONNX operator domains in the DNN wire scanner | Accepted | `security`, `ai`, `dnn`, `fork-local` |
| [ADR-1090](1090-cuda-stream-event-leak-fix.md) | Fix CUDA stream and event leaks on init error paths | Accepted | `cuda`, `security`, `testing` |
| [ADR-1092](1092-framesync-producer-death-deadlock.md) | framesync producer-death deadlock — abort flag + shutdown broadcast | Accepted | `core`, `threading`, `correctness`, `sanitizer`, `fork-local` |
| [ADR-1093](1093-disable-recurring-flaky-tests.md) | Disable two recurring-failure tests via should_fail while root cause is under investigation | Accepted | ci, testing, flaky, picture-pool, sycl, fork-local |
| [ADR-1094](1094-helm-rolling-update-correctness.md) | Helm chart rolling-update correctness — node strategy, PDB default, probe fix, grace period | Accepted | `helm`, `kubernetes`, `deploy`, `fork-local` |
| [ADR-1095](1095-otel-grpc-trace-context.md) | Fix OTel trace context propagation across gRPC boundaries | Accepted | not recorded |
| [ADR-1096](1096-doxygen-private-headers.md) | Doxygen @brief/@param coverage for core internal headers | Accepted | `docs`, `maintainability` |
| [ADR-1097](1097-ai-script-atomic-writes.md) | Atomic file writes for AI-script cache and output files | Accepted | `ai`, `correctness`, `reliability` |
| [ADR-1099](1099-sycl-fsycl-link-propagation.md) | Propagate `-fsycl` via `sycl_dependency` to fix test-binary SIGSEGV | Accepted | not recorded |
| [ADR-1100](1100-feature-extractor-flags-zero-skip-gpu.md) | Skip GPU-flagged extractors when `flags == 0` in `vmaf_get_feature_extractor_by_feature_name` | Accepted | `feature-extractor`, `correctness`, `sycl`, `cuda`, `hip`, `bug-fix`, `fork-local` |
| [ADR-1101](1101-containerfile-gid-uid-2000.md) | Change vmaf container user GID/UID from 1000 to 2000 | Accepted | `build`, `ci`, `workspace` |
| [ADR-1102](1102-phase4b9-container-only-publishing.md) | Container-only canonical artifact publishing (Phase 4b.9) | Accepted | container, build, release, publish, phase4b, docs-policy, fork-local |
| [ADR-1103](1103-hip-vif-mirror2-boundary.md) | Fix integer_vif_hip boundary condition: clamp_i → mirror2_i | Accepted | hip, vif, parity, boundary, correctness, fork-local |
| [ADR-1104](1104-float-vif-avx512-golden-regression-fix.md) | Remove AVX-512 dispatch from float VIF convolution to restore Netflix golden scores | Accepted | `simd`, `correctness`, `float-vif`, `bug-fix` |
| [ADR-1105](1105-ensemble-v2-prod-flip-deferred-oneshot-retrain.md) | `fr_regressor_v2_ensemble` production flip deferred to the one-shot post-RC retrain | Accepted | `ai`, `models`, `rc`, `docs` |
| [ADR-1106](1106-hip-motion-v2-mirror-reflect101-correction.md) | HIP motion_v2 mirror is reflect-101 (`-2`), correcting ADR-0377's `-1` parity claim | Accepted | `hip`, `gpu`, `motion-v2`, `parity`, `boundary`, `bug-fix`, `fork-local` |
| [ADR-1107](1107-threaded-multi-prev-ref-extractor-starvation.md) | Per-extractor `prev_ref` in the threaded batch path (multi-PREV_REF starvation fix) | Accepted | `core`, `threading`, `feature-extractor`, `refcount`, `bug-fix`, `fork-local` |
| [ADR-1108](1108-cuda-motion-v2-motion3-emission.md) | CUDA motion_v2 twin emits motion3_v2_score | Accepted | cuda, feature, parity |
| [ADR-1109](1109-vmafx-node-serve-scoring-grpc.md) | vmafx-node `Serve()` registers the VmafxScoring gRPC service | Accepted | go, node, grpc, scoring, streaming, phase4b, fork-local |
| [ADR-1110](1110-delta-e-itp-metric.md) | Add ΔE-ITP (Delta E ITP) — PQ-only HDR colour-difference CPU extractor | Accepted | metric, feature-extractor, hdr, colour, fork-local |
| [ADR-1112](1112-niqe-nr-metric.md) | NIQE no-reference CPU feature extractor (fork-pkl parity) | Accepted | metrics, feature-extractor, no-reference, cpu, model, fork-local |
| [ADR-1111](1111-pu21-hdr-metric.md) | Add PU21 HDR perceptual metric (PU-PSNR + PU-SSIM, PQ input only) | Accepted | feature-extractor, hdr, pu21, metric, fork-local |
| [ADR-1113](1113-vendor-pelorus-interop-abi.md) | Vendor the Pelorus interop ABI as a pinned read-only mirror | Accepted | interop, abi, vendoring, pelorus, build, ffmpeg, fork-local |
| [ADR-1114](1114-y-funque-plus-atoms.md) | Y-FUNQUE+ wavelet-domain atom features (atoms-only, fused SVR deferred) | Accepted | metrics, feature-extractor, full-reference, cpu, wavelet, license, fork-local |
| [ADR-1115](1115-brisque-nr-metric.md) | BRISQUE no-reference CPU feature extractor (bundled LIVE model) | Accepted | metrics, feature-extractor, no-reference, cpu, model, license, fork-local |
| [ADR-1116](1116-autotune-prefilter-control-plane.md) | vmaf-tune autotune prefilter control plane (Pelorus deband) | Accepted | vmaf-tune, autotune, pelorus, filter-adapters, control-plane, optuna, fork-local |
| [ADR-1117](1117-mcp-tiny-ai-feature-coverage.md) | MCP `vmaf_score` tiny-AI / feature / CTC parameter coverage | Accepted | mcp, ai, docs, agents, fork-local |
| [ADR-1118](1118-perceptual-sidedata-weighting.md) | Pelorus perceptual side-data weights VMAF spatial pooling, golden-isolated and opt-in | Accepted | scoring, pooling, pelorus, interop, ffmpeg, golden-gate, fork-local |
| [ADR-1119](1119-golusoris-go-framework-adoption.md) | Adopt the golusoris fx framework across all vmafx Go binaries | Accepted | go, framework, fx, golusoris, server, controller, node, operator, mcp, vmaf-tune, rc-blocking, fork-local |
| [ADR-1120](1120-pelorus-abi-minor3-resync-complexity.md) | Re-pin the vendored Pelorus interop ABI to minor-3 and consume PEL_SEC_COMPLEXITY in perceptual weighting | Accepted | interop, abi, vendoring, pelorus, scoring, pooling, golden-gate, build, fork-local |
| [ADR-1124](1124-vmafx-tune-go-stage5-per-shot.md) | vmafx-tune-go Stage 5 — per-shot tuning, codec-adapter table, and backend resolution | Accepted | `go`, `vmaf-tune`, `migration`, `codec-adapters`, `cli` |
| [ADR-1125](1125-vmafx-tune-go-port-integration.md) | Reconciling seven independent vmafx-tune Go ports into one tree | Accepted | `go`, `vmafx-tune`, `python-sunset`, `integration`, `fork-local` |
| [ADR-1122](1122-vmaf-v1-model-port.md) | Adopt and port VMAF v1 models (opt-in, v0.6.1 stays default) | Proposed | `vmaf`, `models`, `upstream-port`, `cambi`, `chroma`, `sycl`, `golden-gate` |
| [ADR-1121](1121-sycl-qsv-zerocopy-p010-normalization.md) | SYCL QSV zero-copy — P010 pixel normalization and separate-session decode contract | Accepted | `sycl`, `gpu`, `ffmpeg`, `correctness`, `perf`, `dispatch` |
| [ADR-1126](1126-retire-isort-for-ruff.md) | Retire the standalone isort hook; ruff's `I` rules own import sorting | Accepted | `ci`, `build`, `docs` |
| [ADR-1127](1127-single-semver-release-stream.md) | Use one independent SemVer release stream | Superseded by [ADR-1151](1151-vmafx-first-release-1-0-0.md) | release, semver, automation, docs |
| [ADR-1128](1128-fragment-owned-release-cuts.md) | Make changelog fragments own release cuts | Accepted | release, changelog, automation, ci, docs |
| [ADR-1129](1129-release-container-runtime-alignment.md) | Align release containers with the published tag and runtime ABI | Accepted | release, container, supply-chain, security, mcp, go, gpu, ci |
| [ADR-1138](1138-c-translation-units-keep-null.md) | C translation units keep `NULL`; `modernize-use-nullptr` is scoped to C++ | Accepted | `lint`, `ci`, `c23`, `quality-gate`, `rebase` |
| [ADR-1135](1135-ci-twin-drift-gate.md) | CI twin-drift + stale-source-reference gate | Accepted | ci, build, process, fork-local, claude-rule |
| [ADR-1137](1137-go-dedup-tune-shadow.md) | One implementation per shared Go package — folding the vmafx-tune shadow packages | Accepted | `go`, `vmafx-tune`, `python-sunset`, `refactor`, `fork-local` |
| [ADR-1140](1140-ci-impact-planner.md) | Route required CI work by measured impact instead of pre-declared path filters | Accepted | `ci`, `build`, `docs`, `fork-local` |
| [ADR-1142](1142-whole-codebase-standards.md) | Whole-codebase standards; lint debt only ratchets down | Superseded by [ADR-1267](1267-whole-tree-zero-debt-completion.md) | ci, process, code-quality, agents, cuda, sycl, hip, metal, simd |
| [ADR-1145](1145-neo-stack-derived-from-release.md) | Derive the Intel NEO compute stack (gmmlib and IGC) dynamically from pinned compute-runtime release metadata | Accepted | `build`, `container`, `supply-chain`, `renovate`, `sycl`, `intel` |
| [ADR-1134](1134-vmafx-ort-runner-in-tree.md) | Build `vmafx-ort-runner` in-tree as a cgo shim over libvmaf's DNN session API | Accepted | `go`, `ai`, `onnx`, `build`, `ci`, `container`, `vmafx-tune`, `fork-local` |
| [ADR-1153](1153-twin-dead-sides-resolution.md) | Resolution of Dead .c/.cpp Twin Sides (model.cpp, test_dict.c, test_feature.c) | Accepted | `build`, `ci`, `refactor`, `fork-local` |
| [ADR-1167](1167-adm-cm-row-level-rounding.md) | Row-level rounding accumulator and border row selection for integer ADM GPU kernels | Accepted | cuda, hip, gpu, feature, integer-adm, numerical-correctness |
| [ADR-1141](1141-integer-adm-upstream-mirror-rework.md) | Rework the upstream-mirror integer ADM to the fork lint profile, bit-exact | Accepted | `lint`, `refactor`, `adm`, `simd`, `rebase`, `quality-gate` |
| [ADR-1151](1151-vmafx-first-release-1-0-0.md) | Cut the fork's first release as v1.0.0 on a fresh number line | Accepted | release, semver, automation, ci, docs |
| [ADR-1152](1152-dependency-pr-gate-exemption.md) | Exempt Dependency-Only Bot PRs from Documentation Gates | Accepted | ci, process, docs, dependencies |
| [ADR-1143](1143-cuda-intel-backend-gaps.md) | CUDA and Intel SYCL Backend Gap Closure | Accepted | cuda, sycl, ci, dispatch, python, docs |
| [ADR-1155](1155-tools-upstream-mirror-rework.md) | Rework CLI and Tools Translation Units to Fork Lint Standards | Accepted | tools, cli, clang-tidy, lint, dead-twin, portability, c23, cpp23 |
| [ADR-1146](1146-speed-cambi-upstream-mirror-rework.md) | SPEED and CAMBI Feature Rework to Fork Standards (Bit-Exact) | Accepted | `lint`, `ci`, `refactor`, `feature`, `speed`, `cambi`, `bit-exact` |
| [ADR-1154](1154-hip-backend-gaps.md) | AMD ROCm HIP Backend Gap Closure and Extractor Promotion | Accepted | hip, rocm, gpu, dispatch, parity, docs |
| [ADR-1168](1168-default-model-single-source.md) | The default VMAF model is defined in exactly one place | Accepted | model, ci-gate, c-api, go, python, single-source |
| [ADR-1169](1169-default-model-v1-0-16.md) | The fork's default VMAF model is `vmaf_v1.0.16_3d0h` | Accepted | model, default, v1.0.16, breaking-scores, upstream-divergence |
| [ADR-1166](1166-upstream-issue-harvest.md) | Harvest stale upstream Netflix/vmaf reports, verify each against the fork, fix what still bites | Accepted (Superseded-in-part 2026-09-04 by [ADR-1176](1176-metal-motion-v2-mirror-closeout.md) for Metal motion_v2 mirror deferral) | process, upstream, bug, build, windows, api, docs |
| [ADR-1173](1173-ai-teacher-follows-default-model.md) | AI Teacher Model Follows Default Model Single Source | Accepted | ai, model, single-source, provenance, dataset |
| [ADR-1178](1178-dev-container-image-publish.md) | Dev container image publication and release artifact container enforcement | Accepted | ci, release, supply-chain, container, dev-container, adr-1102, fork-local |
| [ADR-1172](1172-bound-lto-link-parallelism.md) | Bound per-link LTO parallelism to four partitions by default | Accepted | `build`, `meson`, `developer-experience`, `lto` |
| [ADR-1171](1171-release-please-credential-gate-warning.md) | release-please credential gate warns on push, errors on dispatch | Accepted | `ci`, `release`, `release-please`, `supersedes-partial` |
| [ADR-1179](1179-sycl-v1-model-crash-fix.md) | Fix Intel Arc SYCL Crashes and Default Model Resolution Divergence | Accepted | sycl, gpu, cambi, speed, model, default, arc, fp64, adr-0220 |
| [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) | Model options gate GPU twin selection | Accepted | core, feature, options, cuda, gpu-twins, dispatch |
| [ADR-1177](1177-sycl-arc-self-hosted-runner.md) | Containerised self-hosted GitHub Actions runner for Intel Arc SYCL parity CI | Accepted | sycl, gpu, ci, runner, arc-a380, docker |
| [ADR-1176](1176-metal-motion-v2-mirror-closeout.md) | Metal motion_v2 mirror closeout and reflect-101 parity contract | Accepted | `metal`, `gpu`, `motion-v2`, `parity`, `boundary`, `closeout`, `fork-local` |
| [ADR-1197](1197-gpu-threaded-flush-ownership.md) | The threaded flush leaves GPU extractors to their own backend flush | Proposed | cuda, sycl, threading, cli, testing |
| [ADR-1192](1192-netflix-bench-snapshot-drift-not-regenerated.md) | Keep the recorded Netflix benchmark snapshot; do not regenerate it while the GPU paths are broken | Accepted | benchmark, cuda, sycl, testdata, docs |
| [ADR-1195](1195-container-source-revision-guard.md) | Record and verify which source revision the dev container was built from | Proposed | ci, build, testing, agents |
| [ADR-1193](1193-psnr-uncapped-option.md) | Opt-in `uncapped` option splits the PSNR infinity sentinel from the truncation | Accepted | core, feature, psnr, options, cuda, sycl, hip, metal, gpu-twins, upstream |
| [ADR-1191](1191-adm-csf-fixed-point-representability-guard.md) | Integer ADM rejects CSF configurations its fixed-point storage cannot represent | Accepted | `metrics`, `adm`, `cuda`, `sycl`, `hip`, `correctness` |
| [ADR-1190](1190-cli-option-string-escape-grammar.md) | Backslash escapes and a drive-letter affordance in the CLI option-string grammar | Accepted | cli, parser, windows, upstream, bug |
| [ADR-1184](1184-mcp-grpc-bridge-go-only.md) | The MCP gRPC control-plane bridge is Go-only | Accepted | `mcp`, `go`, `grpc`, `agents`, `docs` |
| [ADR-1188](1188-percentile-pooling-methods.md) | Percentile temporal pooling in the public C API | Accepted | core, api, pooling, abi, output-schema, golden-gate |
| [ADR-1196](1196-speed-matmul-simd-dispatch.md) | Dispatch the SpEED dense matrix product through bit-exact AVX2 / AVX-512 kernels | Accepted | simd, feature, performance, testing |
| [ADR-1194](1194-adm-angle-flag-single-source.md) | One integer-ADM `angle_flag` predicate for every backend | Accepted | cuda, hip, sycl, metal, simd, correctness |
| [ADR-1185](1185-backend-perf-baseline-methodology.md) | Per-backend performance baselines are median-of-N, one backend per build dir | Accepted | perf, benchmarks, cuda, sycl, hip, docs |
| [ADR-1198](1198-changelog-unknown-section-is-an-error.md) | An unknown `changelog.d/` subdirectory fails the run instead of warning | Proposed | ci, release, docs, testing |
| [ADR-1199](1199-cuda-picture-handover-barrier.md) | Order caller-written CUDA pictures once per frame, at the dispatch point | Proposed | cuda, correctness, api, testing |
| [ADR-1200](1200-nv-codec-headers-mirror-fallback.md) | The dev container falls back to the GitHub mirror for nv-codec-headers | Proposed | build, supply-chain, ci |
| [ADR-1202](1202-cuda-speed-chroma-4k-launch-bounds.md) | GPU SpEED-chroma twins report singularity separately from failure | Proposed | cuda, sycl, hip, correctness, feature-extractor |
| [ADR-1203](1203-cuda-psnr-hvs-enable-chroma-default.md) | `psnr_hvs_cuda` defaults `enable_chroma` to true, matching every other backend | Proposed | cuda, correctness, feature-extractor, options |
| [ADR-1204](1204-adm-cm-edge-clamp-gpu-twins.md) | GPU ADM contrast-masking twins clamp the far edge instead of mirroring it | Proposed | cuda, sycl, hip, metal, correctness, feature-extractor, testing |
| [ADR-1205](1205-ssimulacra2-fma-unification-scalar-and-gpu.md) | The ssimulacra2 FMA unification extends to the scalar fallback and every GPU host copy | Proposed | cuda, sycl, hip, metal, simd, correctness, feature-extractor, reproducibility |
| [ADR-1206](1206-gpu-parity-large-fixture-variants.md) | Every CUDA parity test also runs against a second, larger fixture | Proposed | testing, cuda, ci, correctness |
| [ADR-1216](1216-gpu-motion3-fps-weight-applied-once.md) | The GPU motion3 twins apply `motion_fps_weight` exactly once | Proposed | `cuda`, `sycl`, `hip`, `correctness`, `feature-extractor`, `testing` |
| [ADR-1217](1217-gpu-float-vif-options-reach-kernel.md) | The GPU float-VIF kernels read `vif_sigma_nsq` and `vif_enhn_gain_limit` from their options | Proposed | `cuda`, `sycl`, `hip`, `correctness`, `feature-extractor`, `testing` |
| [ADR-1218](1218-gpu-speed-singular-device-solution.md) | The GPU SpEED twins zero the device solution and report singularity from the temporal path | Proposed | `cuda`, `sycl`, `hip`, `correctness`, `feature-extractor`, `testing` |
| [ADR-1227](1227-short-workflow-display-names.md) | Workflow display names are short labels; the axis list lives in the file | Accepted | ci, docs, fork-local |
| [ADR-1201](1201-release-candidates-before-1-0-0.md) | Cut release candidates before the final 1.0.0 | Proposed | release, ci, supply-chain |
| [ADR-1225](1225-rocm-10-therock-migration.md) | Migrate the HIP backend to ROCm 10.0.0, installed from digest-pinned container images | Accepted | hip, rocm, build, ci, container, dependencies, fork-local |
| [ADR-1235](1235-pkgconfig-advertises-abi-version.md) | `libvmaf.pc` advertises the ABI version, not the product version | Accepted | `release`, `build`, `ffmpeg`, `abi`, `packaging` |
| [ADR-1223](1223-cuda-ampere-architecture-floor.md) | The CUDA backend requires compute capability 8.0 (Ampere), and CI standardises on CUDA 13.3.1 | Proposed | `cuda`, `build`, `ci`, `docs` |
| [ADR-1237](1237-perf-pass-1245.md) | CAMBI Anti-Dithering AVX2 Vectorization, SpEED SIMD QR Dispatch, and Threaded GPU Flush Alignment | Accepted | perf, simd, x86, hip, cambi, speed |
| [ADR-1229](1229-mcp-go-runtime.md) | The MCP server is the Go binary; the Python package is deprecated | Accepted | mcp, go, python, container, dependencies, fork-local |
| [ADR-1231](1231-base-image-single-source.md) | Container bases and toolchain versions come from one config file | Accepted | build, ci, docs, security |
| [ADR-1240](1240-ffmpeg-release-patch-lifecycle.md) | Maintain the FFmpeg patch stack against stable releases | Accepted | build, ci, ffmpeg |
| [ADR-1238](1238-go-security-required-gate.md) | Require the Go security and test job through impact routing | Accepted | ci, go, security |
| [ADR-1239](1239-agent-cleanup-preserve-work.md) | Preserve work during agent-state cleanup | Accepted | workspace, agents, safety |
| [ADR-1241](1241-worktree-hook-dispatch.md) | Keep Git hooks independent of installer worktrees | Accepted | ci, docs, workspace, agents |
| [ADR-1123](1123-ci-throughput-aggregator-deadline-docker-grouping.md) | Raise the Required-Checks-Aggregator deadline to 240 minutes and batch Docker digest updates | Accepted | `ci`, `renovate`, `dependencies`, `gates`, `fork-local` |
| [ADR-1242](1242-generated-adr-freshness.md) | Require source-owned ADR metadata freshness | Accepted | docs, ci, adr, navigation, automation |
| [ADR-1251](1251-renovate-draft-automerge-deadlock.md) | Renovate opens automerge-eligible and security bumps ready for review | Accepted | ci, dependencies, renovate, merge-train, adr, fork-local |
| [ADR-1236](1236-version-single-source-tree.md) | Single-source package versions and unify Python dependencies | Accepted | build, ci, python, packaging, renovate |
| [ADR-1243](1243-tidy-scoped-baseline-tightening.md) | Allow measured scoped tightening of the lint baseline | Accepted | ci, lint, fork-local |
| [ADR-1244](1244-merge-train-ownership-and-validation.md) | Guard merge-train ownership and exact-head validation | Accepted | ci, agents, safety, fork-local |
| [ADR-1245](1245-cppcheck-exhaustive-configured-analysis.md) | Analyze configured Cppcheck paths exhaustively | Accepted | ci, build, quality |
| [ADR-1246](1246-cppcheck-public-entrypoints.md) | Model verified public functions as Cppcheck entrypoints | Accepted | ci, build, quality |
| [ADR-1248](1248-repository-security-enforcement.md) | Enforce repository security through public rulesets | Superseded by [ADR-1252](1252-solo-maintainer-declared-bypass.md) | security, ci, governance |
| [ADR-1252](1252-solo-maintainer-declared-bypass.md) | Declare the single maintainer's bypass actor | Accepted, Supersedes [ADR-1248](1248-repository-security-enforcement.md) | security, ci, governance, adr, fork-local |
| [ADR-1253](1253-scalar-fma-not-fused-on-msvcrt.md) | Scalar references compute their fused multiply-add themselves | Accepted | `simd`, `build`, `windows` |
| [ADR-1219](1219-gpu-cambi-tvi-shared-bisection.md) | The HIP and Metal CAMBI twins use the shared TVI bisection and the CPU's border rules | Proposed | `hip`, `metal`, `correctness`, `feature-extractor`, `testing` |
| [ADR-1220](1220-gpu-float-adm-options-reach-kernels.md) | The GPU float-ADM kernels honour `adm_p_norm`, `adm_bypass_cm` and `adm_skip_scale0` | Proposed | `cuda`, `sycl`, `hip`, `metal`, `correctness`, `feature-extractor`, `testing` |
| [ADR-1221](1221-gpu-ms-ssim-db-ceiling.md) | `clip_db` is a ceiling on the MS-SSIM dB output, not a clamp on the linear score | Proposed | `cuda`, `sycl`, `hip`, `correctness`, `feature-extractor`, `testing` |
| [ADR-1226](1226-cuda-adm-cm-aim-grid-occupancy.md) | Size the CUDA AIM CM launch by SM count, not by a fixed rows-per-thread | Accepted | cuda, performance, adm, fork-local |
| [ADR-1230](1230-modern-gcc-toolchain.md) | The CI gcc moves forward with clang and meson, and the ratchet records it | Accepted | ci, build, tooling, clang-tidy, fork-local |
| [ADR-1233](1233-github-native-release-notes.md) | GitHub generates the release body; CHANGELOG.md keeps an index | Accepted | ci, docs, build |
| [ADR-1209](1209-cli-gpumask-negative-contract.md) | `--gpumask` keeps rejecting negative values; the test script uses a positive mask | Proposed | cli, testing, upstream-divergence, correctness |
| [ADR-1211](1211-hip-integer-adm-picture-staging.md) | `integer_adm_hip` stages the luma plane onto the device before launching | Proposed | hip, correctness, feature-extractor, adm |
| [ADR-1215](1215-cuda-psnr-16bpc-plane-argument.md) | The 16-bpc CUDA PSNR kernel takes the plane index the host has always passed | Proposed | cuda, correctness, feature-extractor, bit-depth |
| [ADR-1212](1212-gpu-moment-bit-depth-normalisation.md) | The GPU `float_moment` twins normalise by the bit-depth scaler on the host | Proposed | cuda, sycl, hip, correctness, feature-extractor, bit-depth |
| [ADR-1213](1213-hip-ciede-chroma-ceil-dimensions.md) | `ciede_hip` sizes its chroma staging with the picture's ceil dimensions | Proposed | hip, correctness, feature-extractor, memory-safety |
| [ADR-1210](1210-sycl-integer-adm-cm-near-edge-mirror.md) | The SYCL integer-ADM contrast-masking kernel mirrors its near edge | Proposed | sycl, correctness, feature-extractor, adm |
| [ADR-1207](1207-feature-isa-invariance-gate.md) | A test gates every feature's score against the host instruction set | Proposed | testing, simd, correctness, ci, reproducibility |
| [ADR-1208](1208-ssimulacra2-edge-diff-double-subtract.md) | The ssimulacra2 edge-diff SIMD loops take their difference in double | Proposed | simd, correctness, feature-extractor, reproducibility |
| [ADR-1222](1222-code-scanning-alert-triage-and-scope.md) | In-code suppressions do not close code-scanning alerts; scope the scan instead | Proposed | `ci`, `security`, `docs`, `mcp` |
| [ADR-1224](1224-cuda-tile-not-adopted.md) | CUDA Tile C++ is not adopted; the audit's incidental findings are | Proposed | `cuda`, `performance`, `build`, `correctness` |
| [ADR-1228](1228-upstream-ab-perf-milestone.md) | A recurring "faster than upstream, and still exact" milestone | Accepted | performance, benchmarking, cuda, sycl, hip, process, fork-local |
| [ADR-1234](1234-local-preflight-gate.md) | The local gate builds with every compiler CI does | Accepted | ci, build, agents |
| [ADR-1254](1254-win64-cannot-realign-the-stack.md) | Wide vector register pressure is a Win64 correctness constraint, not a performance one | Accepted | simd, build, windows, ci |
| [ADR-1255](1255-spdx-residual-identifier-correction.md) | Correct the SPDX identifiers PR #1457 does not reach, to the licence this repository already declares | Accepted | license, docs, build |
| [ADR-1250](1250-eupl-fork-relicense.md) | Fork-authored code moves to EUPL-1.2; code that carries someone else's work does not | Proposed | license, compliance, process, docs, breaking-change |
| [ADR-1249](1249-praetor-governance-adoption.md) | Adopt praetor governance, with lefthook owning the git hooks | Proposed | ci, process, agents, tooling, governance, docs, workspace |
| [ADR-1256](1256-cambi-spatial-mask-simd-dispatch.md) | Dispatch CAMBI's spatial-mask row SIMD kernels only where they measurably beat scalar | Accepted | simd, avx2, avx512, neon, cambi, perf, upstream-port, fork-local |
| [ADR-1261](1261-mypy-pre-push-delta-gate.md) | The local type-check hook fails on findings a branch introduces, not on ones it inherits | Proposed | ci, hooks, python, tooling, fork-local |
| [ADR-1258](1258-keep-64-bit-only-retire-i686-lane.md) | Keep the fork 64-bit only; retire the resurrected i686 lane | Accepted, Supersedes [ADR-0151](0151-i686-ci-netflix-1481.md) | build, ci, x86, netflix-upstream, fork-local |
| [ADR-1259](1259-ci-build-matrix-as-it-runs.md) | Record the CI build matrix as it actually runs | Accepted, Supersedes | ci, build, fork-local |
| [ADR-1260](1260-windows-arm64-cpu-lane.md) | Windows on ARM64 CPU build-and-test lane | Proposed | ci, build, arm64, windows, simd, fork-local |
| [ADR-1264](1264-hip-scaffold-enosys-contract.md) | The HIP scaffold posture reports `-ENOSYS`, and its tests check both sites | Proposed | hip, testing, scaffold, gpu, fork-local |
| [ADR-1263](1263-hip-platform-macro-single-source.md) | `__HIP_PLATFORM_AMD__` is declared once by the build, not by each source | Proposed | hip, build, meson, warnings, fork-local |
| [ADR-1262](1262-cli-input-read-error-exit-code.md) | A failed input read exits 102; a legitimately shorter stream stays exit 0 | Proposed | cli, tools, exit-codes, fork-local |
| [ADR-1257](1257-retire-darwin-adm-dwt2-legacy-dispatch.md) | Retire the Darwin three-tap integer-ADM DWT2 compatibility dispatch | Accepted | `simd`, `neon`, `integer-adm`, `darwin`, `testing`, `upstream-sync` |
| [ADR-1265](1265-clang-tidy-header-filter-absolute-paths.md) | The clang-tidy header filter matches absolute paths, so headers count | Proposed | ci, clang-tidy, lint, ratchet, fork-local |
| [ADR-1214](1214-float-adm-csf-scale-watson-mode-and-aliases.md) | The float-ADM GPU twins ignore `adm_csf_scale` in Watson mode and share the CPU's option aliases | Proposed | cuda, sycl, hip, metal, correctness, feature-extractor, options |
| [ADR-1266](1266-gpu-nolint-citation-closeout-round-2.md) | Exact-comment NOLINT citation scan and tree-wide closeout | Accepted | lint, cleanup, touched-file-rule, sycl, cuda, hip, ci |
| [ADR-1267](1267-whole-tree-zero-debt-completion.md) | Whole-tree warning and standards debt must reach zero | Accepted | ci, lint, cleanup, code-quality, agents, cuda, sycl, hip, simd |
| [ADR-1247](1247-scorecard-exact-head-gates.md) | Bind Scorecard gates to their measured source and scope | Accepted | ci, security, supply-chain |
