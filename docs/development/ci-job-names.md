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
   `Tidy SYCL`).
4. **Platform consistency**: Operating system matrix legs follow a uniform
   prefix pattern (e.g. `Ubuntu gcc+DNN`, `Ubuntu clang+DNN`, `Ubuntu HIP`,
   `Windows MinGW64`, `Windows MSVC+CUDA`, `Windows MSVC+SYCL`).

## Workflow display names (ADR-1227)

The same $\le 30$ character budget applies to workflow-level `name:` fields,
for a sharper reason: GitHub's `badge.svg` endpoint paints the **workflow
name** into the badge, so the seven status badges at the top of
[`README.md`](../../README.md) render whatever the workflow is called. Names
like `Tests & Quality Gates — Netflix Golden / Sanitizers / Tiny AI /
Coverage` (72 characters) turned those badges into banners that wrapped the
README header across several lines.

The four workflows the README badges point at are named exactly as their badge
link labels — `Tests`, `Security`, `Builds`, `FFmpeg` — so the badge text and
the link text agree.

The axis list a workflow used to carry in its name lives in a comment directly
under the `name:` line instead, where it does not have to fit in a badge:

```yaml
name: Security
# Security — Semgrep / CodeQL / Gitleaks / Dependency Review.
# The display name is deliberately short: GitHub's badge.svg paints it
# into the README status badge. See ADR-1227.
```

**Filenames are not part of this budget.** Badge and Actions URLs are keyed on
the filename, so filenames stay purpose-descriptive kebab-case per
[ADR-0116](../adr/0116-ci-workflow-naming-convention.md) §1 — renaming one
means re-pointing every badge.

## Aggregator gating

Branch protection targets a single context: `Required Checks Aggregator` in
[`.github/workflows/required-aggregator.yml`](../../.github/workflows/required-aggregator.yml).
The aggregator's `required` list holds 79 check names defined across 20
workflows; each run evaluates 78 of them. It declares both Scorecard event
gates, then removes the non-applicable one so only `Scorecard PR Gate` or
`Scorecard Master Gate` is evaluated for a run.

A required name must be reported by exactly one job, because the aggregator
keeps one check run per name, so two jobs sharing a name can mask each other's
failure. `scripts/ci/check-aggregator-names.sh` enforces it. The `build.yml`
Windows row used to share `Windows MSVC+CUDA` with the required
`libvmaf-build-matrix.yml` lane and is now `Windows MSVC+CUDA (full)`
([ADR-1259](../adr/1259-ci-build-matrix-as-it-runs.md)).

To prevent drift between workflow job definitions and the aggregator's required
check array, all required checks are tagged in their defining workflow with
`# required-aggregator` and verified by
[`scripts/ci/check-aggregator-names.sh`](../../scripts/ci/check-aggregator-names.sh).
The gate is wired into:

- `make lint-sh`
- `.pre-commit-config.yaml` (`check-aggregator-names` hook)
- CI pre-commit and lint stages

## Complete mapping table

"Previous Name" is the display name before PR #1286 (`f93a0037f`) shortened
it; "unchanged" means that PR did not rename the job. The table covers every
job the PR renamed, every required check and every build lane.

| Workflow | Previous Name | Shortened Name | Length | Required |
| --- | --- | --- | --- | --- |
| `libvmaf-build-matrix.yml` | `Build — Ubuntu gcc (CPU) + DNN` | `Ubuntu gcc+DNN` | 14 | Yes |
| `libvmaf-build-matrix.yml` | `Build — Ubuntu clang (CPU) + DNN` | `Ubuntu clang+DNN` | 16 | Yes |
| `libvmaf-build-matrix.yml` | `Build — Ubuntu HIP (T7-10b runtime)` | `Ubuntu HIP` | 10 | Yes |
| `libvmaf-build-matrix.yml` | `Build — Windows MinGW64 (CPU)` | `Windows MinGW64` | 15 | Yes |
| `libvmaf-build-matrix.yml` | `Build — Windows MSVC + CUDA (build only)` | `Windows MSVC+CUDA` | 18 | Yes |
| `libvmaf-build-matrix.yml` | `Build — Windows MSVC + oneAPI SYCL (build only)` | `Windows MSVC+SYCL` | 18 | Yes |
| `libvmaf-build-matrix.yml` | `Build — Ubuntu gcc (CPU)` | `Ubuntu gcc` | 10 | No |
| `libvmaf-build-matrix.yml` | `Build — Ubuntu clang (CPU)` | `Ubuntu clang` | 12 | No |
| `libvmaf-build-matrix.yml` | `Build — macOS clang (CPU)` | `macOS clang` | 11 | No |
| `libvmaf-build-matrix.yml` | `Build — Ubuntu ARM clang (CPU)` | `Ubuntu ARM clang` | 16 | No |
| `libvmaf-build-matrix.yml` | `Build — macOS clang (CPU) + DNN` | `macOS clang+DNN` | 15 | No |
| `libvmaf-build-matrix.yml` | `Build — macOS Metal (T8-1 scaffold)` | `macOS Metal` | 11 | No |
| `libvmaf-build-matrix.yml` | `Build — Ubuntu gcc Static (CPU)` | `Ubuntu gcc static` | 17 | No |
| `libvmaf-build-matrix.yml` | `Build — Ubuntu CUDA Static` | `Ubuntu CUDA static` | 18 | No |
| `libvmaf-build-matrix.yml` | `Build — Ubuntu SYCL` | `Ubuntu SYCL` | 11 | No |
| `libvmaf-build-matrix.yml` | `Build — Ubuntu CUDA` | `Ubuntu CUDA` | 11 | No |
| `libvmaf-build-matrix.yml` | `Build — Ubuntu SYCL + CUDA` | `Ubuntu SYCL+CUDA` | 16 | No |
| `lint-and-format.yml` | `Pre-Commit (Formatters + Basic Checks)` | `Pre-Commit` | 10 | Yes |
| `libvmaf-build-matrix.yml` | new in ADR-1260 | `Windows ARM64 MSVC` | 18 | No |
| `lint-and-format.yml` | `Clang-Tidy (Changed C/C++ Files)` | `Tidy Changed` | 12 | Yes |
| `lint-and-format.yml` | `Clang-Tidy Ratchet (Whole Tree)` | `Tidy Ratchet` | 12 | Yes |
| `lint-and-format.yml` | `Cppcheck (Whole Project)` | `Cppcheck` | 8 | Yes |
| `lint-and-format.yml` | `Python Lint (Ruff + Black + mypy)` | `Python Lint` | 11 | Yes |
| `lint-and-format.yml` | unchanged | `Docs` | 4 | Yes |
| `lint-and-format.yml` | `Twin Drift + Stale Source Refs (ADR-1135)` | `Twin Drift` | 10 | Yes |
| `lint-and-format.yml` | `ShellCheck + shfmt (All *.sh)` | `ShellCheck + shfmt` | 17 | Yes |
| `lint-and-format.yml` | `Clang-Tidy SYCL (Changed Files, Advisory)` | `Tidy SYCL` | 9 | Yes |
| `lint-and-format.yml` | `Check — No committed conflict markers` | `No Conflict Markers` | 19 | No |
| `lint-and-format.yml` | `Markdown lint (markdownlint-cli2)` | `Markdown Lint` | 13 | No |
| `rule-enforcement.yml` | `Deep-Dive Deliverables Checklist (ADR-0108)` | `Deliverables Checklist` | 22 | Yes |
| `rule-enforcement.yml` | `Doc-Substance Gate (ADR-0100 / 0167)` | `Doc-Substance Gate` | 18 | Yes |
| `rule-enforcement.yml` | `docs/state.md Touch Gate (ADR-0165)` | `docs/state.md Gate` | 18 | Yes |
| `rule-enforcement.yml` | `FFmpeg-Patches Surface Sync (CLAUDE.md §12 r14, ADR-0356)` | `FFmpeg-Patches Surface Sync` | 27 | Yes |
| `rule-enforcement.yml` | `ADR Number Collision Guard (ADR-0386 / ADR-0628)` | `ADR Collision Guard` | 19 | Yes |
| `rule-enforcement.yml` | `Release Script Contract (ADR-1128)` | `Release Script Contract` | 23 | Yes |
| `rule-enforcement.yml` | `ADR-Backfill Advisory (ADR-0106)` | `ADR-Backfill Advisory` | 21 | No |
| `security-scans.yml` | `Semgrep (CWE Top 25 + CERT-C + Custom)` | `Semgrep` | 7 | Yes |
| `security-scans.yml` | unchanged | `CodeQL (C/C++)` | 15 | Yes |
| `security-scans.yml` | unchanged | `CodeQL (Python)` | 15 | Yes |
| `security-scans.yml` | unchanged | `CodeQL (Actions)` | 16 | Yes |
| `security-scans.yml` | unchanged | `CodeQL` | 6 | Yes |
| `security-scans.yml` | `Gitleaks (Secret Scan)` | `Gitleaks` | 8 | Yes |
| `security-scans.yml` | `Dependency Review (PR Diff)` | `Dependency Review` | 17 | Yes |
| `tests-and-quality-gates.yml` | `Netflix CPU Golden Tests (D24)` | `Netflix CPU Golden` | 18 | Yes |
| `tests-and-quality-gates.yml` | `Sanitizers — ASan + UBSan + MSan (address)` | `Sanitizers (address)` | 20 | Yes |
| `tests-and-quality-gates.yml` | `Sanitizers — ASan + UBSan + MSan (thread)` | `Sanitizers (thread)` | 19 | Yes |
| `tests-and-quality-gates.yml` | `Sanitizers — ASan + UBSan + MSan (undefined)` | `Sanitizers (undefined)` | 22 | Yes |
| `tests-and-quality-gates.yml` | `Tiny AI (DNN Suite + ai/ Pytests)` | `Tiny AI` | 7 | Yes |
| `tests-and-quality-gates.yml` | `SYCL float_ssim Parity (Arc DG2-G10)` | `SYCL float_ssim Parity` | 23 | Yes |
| `tests-and-quality-gates.yml` | `Assertion Density (Power of 10 §5)` | `Assertion Density` | 17 | Yes |
| `tests-and-quality-gates.yml` | `Coverage Gate (Ramping to 70% / 85% Critical)` | `Coverage Gate` | 13 | No |
| `tests-and-quality-gates.yml` | `Coverage Gate — GPU Backends (Advisory)` | `Coverage GPU` | 12 | Yes |
| `tests-and-quality-gates.yml` | `MCP Smoke (Embedded C + Python Server)` | `MCP Smoke` | 9 | No |
| `go-ci.yml` | unchanged | `go vet + go test` | 16 | Yes |
| `scorecard-policy.yml` | added after #1286 | `Scorecard PR Gate` | 17 | Yes |
| `scorecard.yml` | added after #1286 | `Scorecard Master Gate` | 21 | Yes |
| `sycl-parity.yml` | added after #1286 | `SYCL Parity (Arc A380)` | 22 | Yes |
| `ffmpeg-patch-stack.yml` | added after #1286 | `FFmpeg Patch Stack` | 18 | Yes |
| `standards-gate.yml` | added after #1286 | `Standards & Invariant Verification Gate` | 39 | Yes |
| `build.yml` | `Build — Linux (Intel LLVM, all backends)` | `Linux Intel LLVM` | 16 | No |
| `build.yml` | `Build — macOS (Clang, CPU + Metal)` | `macOS Clang+Metal` | 17 | No |
| `build.yml` | `Build — Windows (MSVC + CUDA)` | `Windows MSVC+CUDA (full)` | 24 | No |
| `ffmpeg-integration.yml` | `FFmpeg — Ubuntu gcc (Build Only)` | `FFmpeg Ubuntu gcc` | 17 | No |
| `ffmpeg-integration.yml` | `FFmpeg — macOS clang (Build Only)` | `FFmpeg macOS clang` | 18 | No |
| `ffmpeg-integration.yml` | `FFmpeg — SYCL (Build Only)` | `FFmpeg SYCL` | 11 | No |
| `sanitizers.yml` | `Sanitizers — ASan + UBSan (PR gate)` | `Sanitizers ASan+UBSan` | 20 | No |
| `sanitizers.yml` | `Sanitizers — TSan (master push)` | `Sanitizers TSan` | 15 | No |
| `sanitizers.yml` | `Fuzz — ${{ matrix.target }} (nightly)` | `Fuzz ${{ matrix.target }}` | <=25 | No |
| `rust-ci.yml` | `vmafx-sys (fmt + clippy + test)` | `vmafx-sys CI` | 12 | No |
| `rust-ci.yml` | `cargo-deny (licenses, bans, advisories, sources)` | `cargo-deny` | 10 | No |
| `supply-chain.yml` | `Validate ordinary tag and coordinated versions` | `Validate release versions` | 25 | No |
| `supply-chain.yml` | `Build Linux release artifacts (libvmaf.so chain, vmaf CLI, models)` | `Build Linux artifacts` | 21 | No |
| `supply-chain.yml` | `Verify downloaded Linux release runtime` | `Verify Linux runtime` | 20 | No |
| `supply-chain.yml` | `Generate libvmaf + vmaf-mcp SBOMs (SPDX + CycloneDX)` | `Generate SBOMs` | 14 | No |
| `supply-chain.yml` | `Sigstore keyless sign (release artifacts + SBOMs)` | `Sigstore sign artifacts` | 23 | No |
| `supply-chain.yml` | `SLSA L3 provenance — libvmaf artifacts` | `SLSA libvmaf` | 12 | No |
| `supply-chain.yml` | `Sigstore keyless sign — vmaf-mcp` | `Sigstore sign vmaf-mcp` | 22 | No |
| `supply-chain.yml` | `SLSA L3 provenance — vmaf-mcp distributions` | `SLSA vmaf-mcp` | 12 | No |
| `supply-chain.yml` | `Publish vmaf-mcp to PyPI (Trusted Publishing)` | `Publish vmaf-mcp PyPI` | 20 | No |
| `supply-chain.yml` | `Attach SBOM + signatures to GitHub Release` | `Attach release assets` | 21 | No |
| `docker-publish-operator-node.yml` | `Validate published ordinary tag` | `Validate tag` | 12 | No |
| `docker-publish-operator-node.yml` | `Build + push vmafx-operator (amd64 + arm64)` | `Publish vmafx-operator` | 22 | No |
| `docker-publish-operator-node.yml` | `Build + push vmafx-server (amd64 + arm64)` | `Publish vmafx-server` | 20 | No |
| `docker-publish-operator-node.yml` | `Build + push vmafx-node CPU (amd64 + arm64)` | `Publish vmafx-node CPU` | 21 | No |
| `docker-publish-operator-node.yml` | `Smoke-test operator + server + node images` | `Smoke-test images` | 17 | No |
| `docker-publish-operator-node.yml` | `All Go service images published` | `Images published` | 16 | No |
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
