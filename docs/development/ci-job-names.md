# CI job display names

This document defines the naming convention and inventory of GitHub Actions CI
job display names across [`.github/workflows/`](../../.github/workflows/).

## Naming convention

CI job and matrix display names (`name:`) conform to the following guidelines:

1. **Length budget**: All job display names target $\le 30$ characters for
   legible rendering in GitHub's check-run list, mobile views, and CLI tools
   (`gh pr checks`).
2. **Concise identifiers**: Redundant parentheticals, verbose descriptive
   suffixes, and inline policy or ADR citations (e.g. `(ADR-0108)`, `(D24)`,
   `(CERT C ENV33-C / JPL Rule 5)`) are removed. The governing ADR or standard
   remains documented in workflow comments and repository documentation rather
   than the check name itself.
3. **Discriminative qualifiers**: Essential matrix qualifiers remain intact in
   compact form (e.g. `Sanitizers (address)`, `CodeQL (C/C++)`,
   `Tidy SYCL (advisory)`).
4. **Platform consistency**: Operating system matrix legs follow a uniform
   prefix pattern (e.g. `Ubuntu gcc+DNN`, `Ubuntu clang+DNN`, `Ubuntu HIP`,
   `Windows MinGW64`, `Windows MSVC+CUDA`, `Windows MSVC+SYCL`).

## Aggregator gating

Branch protection targets a single context: `Required Checks Aggregator` in
[`.github/workflows/required-aggregator.yml`](../../.github/workflows/required-aggregator.yml).
The aggregator monitors 34 required checks defined across five workflows.

To prevent drift between workflow job definitions and the aggregator's required
check array, all required checks are tagged in their defining workflow with
`# required-aggregator` and verified by
[`scripts/ci/check-aggregator-names.sh`](../../scripts/ci/check-aggregator-names.sh).
The gate is wired into:

- `make lint-sh`
- `.pre-commit-config.yaml` (`check-aggregator-names` hook)
- CI pre-commit and lint stages

## Complete mapping table

| Workflow | Previous Name | Shortened Name | Length | Required |
| --- | --- | --- | --- | --- |
| `libvmaf-build-matrix.yml` | `Linux CI (GCC + OpenVINO DNN)` | `Ubuntu gcc+DNN` | 14 | Yes |
| `libvmaf-build-matrix.yml` | `Linux CI (Clang + OpenVINO DNN)` | `Ubuntu clang+DNN` | 16 | Yes |
| `libvmaf-build-matrix.yml` | `Linux CI (AMD ROCm / HIP)` | `Ubuntu HIP` | 10 | Yes |
| `libvmaf-build-matrix.yml` | `Windows CI (MinGW-w64 GCC)` | `Windows MinGW64` | 15 | Yes |
| `libvmaf-build-matrix.yml` | `Windows CI (MSVC + CUDA 13)` | `Windows MSVC+CUDA` | 18 | Yes |
| `libvmaf-build-matrix.yml` | `Windows CI (MSVC + Intel oneAPI SYCL)` | `Windows MSVC+SYCL` | 18 | Yes |
| `lint-and-format.yml` | `Pre-Commit All-Files Hygiene Gate` | `Pre-Commit` | 10 | Yes |
| `lint-and-format.yml` | `Clang-Tidy (Changed C/C++ Files)` | `Tidy Changed` | 12 | Yes |
| `lint-and-format.yml` | `Clang-Tidy Ratchet (Whole Tree)` | `Tidy Ratchet` | 12 | Yes |
| `lint-and-format.yml` | `C/C++ Static Analysis (cppcheck)` | `Cppcheck` | 8 | Yes |
| `lint-and-format.yml` | `Python Linters (black + ruff + mypy)` | `Python Lint` | 11 | Yes |
| `lint-and-format.yml` | `Docs Build (Doxygen + Sphinx)` | `Docs` | 4 | Yes |
| `lint-and-format.yml` | `Twin Drift (C/C++ Source Parity Gate)` | `Twin Drift` | 10 | Yes |
| `lint-and-format.yml` | `Shell Scripts (ShellCheck + shfmt)` | `ShellCheck + shfmt` | 17 | Yes |
| `lint-and-format.yml` | `Clang-Tidy (Full SYCL Codebase — Advisory)` | `Tidy SYCL (advisory)` | 20 | No |
| `lint-and-format.yml` | `No Conflict Markers Staged / Committed` | `No Conflict Markers` | 19 | No |
| `lint-and-format.yml` | `Markdown Lint (ADR-0866, changed files)` | `Markdown Lint` | 13 | No |
| `rule-enforcement.yml` | `Deep-Dive Deliverables Checklist Gate` | `Deliverables Checklist` | 22 | Yes |
| `rule-enforcement.yml` | `Doc-Substance Gate (ADR-0100 / ADR-0042)` | `Doc-Substance Gate` | 18 | Yes |
| `rule-enforcement.yml` | `docs/state.md Freshness Gate` | `docs/state.md Gate` | 18 | Yes |
| `rule-enforcement.yml` | `FFmpeg-Patches libvmaf Surface Parity Gate` | `FFmpeg-Patches Surface Sync` | 27 | Yes |
| `rule-enforcement.yml` | `ADR Number Collision Guard (ADR-0386 / ADR-0628)` | `ADR Collision Guard` | 19 | Yes |
| `rule-enforcement.yml` | `Release Script Contract Tests` | `Release Script Contract` | 23 | Yes |
| `rule-enforcement.yml` | `ADR-Backfill Scope Check (Advisory)` | `ADR-Backfill Advisory` | 21 | No |
| `security-scans.yml` | `Semgrep Code Quality & Security Scan` | `Semgrep` | 7 | Yes |
| `security-scans.yml` | `CodeQL (C/C++ Analysis)` | `CodeQL (C/C++)` | 15 | Yes |
| `security-scans.yml` | `CodeQL (Python Analysis)` | `CodeQL (Python)` | 15 | Yes |
| `security-scans.yml` | `CodeQL (GitHub Actions Analysis)` | `CodeQL (Actions)` | 16 | Yes |
| `security-scans.yml` | `CodeQL (Unified Analysis Status)` | `CodeQL` | 6 | Yes |
| `security-scans.yml` | `Gitleaks Secrets Scan` | `Gitleaks` | 8 | Yes |
| `security-scans.yml` | `Dependency Review (PR Dependency Changes)` | `Dependency Review` | 17 | Yes |
| `tests-and-quality-gates.yml` | `Netflix Golden Data (x86_64 CPU)` | `Netflix CPU Golden` | 18 | Yes |
| `tests-and-quality-gates.yml` | `Sanitizers (AddressSanitizer + LeakSanitizer)` | `Sanitizers (address)` | 20 | Yes |
| `tests-and-quality-gates.yml` | `Sanitizers (ThreadSanitizer)` | `Sanitizers (thread)` | 19 | Yes |
| `tests-and-quality-gates.yml` | `Sanitizers (UndefinedBehaviorSanitizer)` | `Sanitizers (undefined)` | 22 | Yes |
| `tests-and-quality-gates.yml` | `Tiny AI Model Artifacts & Architecture Tests` | `Tiny AI` | 7 | Yes |
| `tests-and-quality-gates.yml` | `SYCL float_ssim Parity Gate` | `SYCL float_ssim Parity` | 23 | Yes |
| `tests-and-quality-gates.yml` | `Assertion Density (CERT C ENV33-C / JPL Rule 5)` | `Assertion Density` | 17 | Yes |
| `tests-and-quality-gates.yml` | `Coverage Gate (C Engine + Python Core)` | `Coverage Gate` | 13 | No |
| `tests-and-quality-gates.yml` | `Cross-Backend Parity (Floating-Point ULP Gate)` | `Cross-Backend ULP Diff` | 22 | No |
| `tests-and-quality-gates.yml` | `Coverage (C Engine GPU Twins — Advisory)` | `Coverage GPU (advisory)` | 23 | No |
| `tests-and-quality-gates.yml` | `MCP Server Smoke Test (stdio / JSON-RPC)` | `MCP Smoke` | 9 | No |
| `build.yml` | `Linux Intel LLVM / SYCL (Arc GPU / Level Zero)` | `Linux Intel LLVM` | 16 | No |
| `build.yml` | `macOS Clang (Metal GPU)` | `macOS Clang+Metal` | 17 | No |
| `build.yml` | `Windows MSVC (CUDA 13 GPU)` | `Windows MSVC+CUDA` | 18 | No |
| `ffmpeg-integration.yml` | `FFmpeg Integration (Ubuntu gcc)` | `FFmpeg Ubuntu gcc` | 17 | No |
| `ffmpeg-integration.yml` | `FFmpeg Integration (macOS clang)` | `FFmpeg macOS clang` | 18 | No |
| `ffmpeg-integration.yml` | `FFmpeg Integration (Ubuntu icpx SYCL)` | `FFmpeg SYCL` | 11 | No |
| `sanitizers.yml` | `Sanitizers (AddressSanitizer + UndefinedBehaviorSanitizer)` | `Sanitizers ASan+UBSan` | 20 | No |
| `sanitizers.yml` | `Sanitizers (ThreadSanitizer Data Race Detection)` | `Sanitizers TSan` | 15 | No |
| `sanitizers.yml` | `Fuzz Target Regression Suite (${{ matrix.target }})` | `Fuzz ${{ matrix.target }}` | <=25 | No |
| `rust-ci.yml` | `vmafx-sys Rust FFI Bindings & Integration Tests` | `vmafx-sys CI` | 12 | No |
| `rust-ci.yml` | `Cargo Deny (Licenses + Advisories + Bans)` | `cargo-deny` | 10 | No |
| `supply-chain.yml` | `Validate release version metadata` | `Validate release versions` | 25 | No |
| `supply-chain.yml` | `Build multi-platform release artifacts` | `Build Linux artifacts` | 21 | No |
| `supply-chain.yml` | `Verify release binary runtime compatibility` | `Verify Linux runtime` | 20 | No |
| `supply-chain.yml` | `Generate CycloneDX SBOMs and hashes` | `Generate SBOMs` | 14 | No |
| `supply-chain.yml` | `Sigstore keyless signing of release artifacts` | `Sigstore sign artifacts` | 23 | No |
| `supply-chain.yml` | `Generate SLSA provenance for libvmaf artifacts` | `SLSA libvmaf` | 12 | No |
| `supply-chain.yml` | `Sigstore keyless signing of vmaf-mcp wheels` | `Sigstore sign vmaf-mcp` | 22 | No |
| `supply-chain.yml` | `Generate SLSA provenance for vmaf-mcp wheel` | `SLSA vmaf-mcp` | 12 | No |
| `supply-chain.yml` | `Publish vmaf-mcp to PyPI via Trusted Publishing` | `Publish vmaf-mcp PyPI` | 20 | No |
| `supply-chain.yml` | `Attach SLSA provenance and SBOM to GitHub Release` | `Attach release assets` | 21 | No |
| `docker-publish-operator-node.yml` | `Validate published ordinary tag` | `Validate tag` | 12 | No |
| `docker-publish-operator-node.yml` | `Build + push vmafx-operator image (amd64 + arm64)` | `Publish vmafx-operator` | 22 | No |
| `docker-publish-operator-node.yml` | `Build + push vmafx-server image (amd64 + arm64)` | `Publish vmafx-server` | 20 | No |
| `docker-publish-operator-node.yml` | `Build + push vmafx-node-cpu image (amd64 + arm64)` | `Publish vmafx-node CPU` | 21 | No |
| `docker-publish-operator-node.yml` | `Smoke-test operator/node image entrypoints` | `Smoke-test images` | 17 | No |
| `docker-publish-operator-node.yml` | `All operator/node images published` | `Images published` | 16 | No |
| `docker-publish-production.yml` | `Validate published ordinary tag` | `Validate tag` | 12 | No |
| `docker-publish-production.yml` | `Build + push CPU image (amd64 + arm64)` | `Publish CPU image` | 17 | No |
| `docker-publish-production.yml` | `Build + push CUDA 13 image (amd64)` | `Publish CUDA 13 image` | 21 | No |
| `docker-publish-production.yml` | `Build + push ROCm 7 image (amd64)` | `Publish ROCm 7 image` | 19 | No |
| `docker-publish-production.yml` | `Build + push oneAPI 2025 image (amd64)` | `Publish oneAPI 2025 image` | 24 | No |
| `docker-publish-production.yml` | `Smoke-test GPU image entrypoints` | `Smoke-test GPU images` | 21 | No |
| `docker-publish-production.yml` | `Build + push MCP server image (amd64 + arm64)` | `Publish MCP server image` | 24 | No |
| `docker-publish-production.yml` | `All production images published` | `Images published` | 16 | No |
| `upstream-watcher.yml` | `FFmpeg av1_videotoolbox encoder` | `FFmpeg av1_videotoolbox` | 23 | No |
| `dev-container-build.yml` | `Dev Container Build + Smoke Test` | `Dev Container Build` | 19 | No |
| `required-aggregator.yml` | `Required Checks Aggregator` | `Required Checks Aggregator` | 25 | Status |
