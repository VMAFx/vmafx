<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3336: Dev Image Reverse Debugger (rr) and Build-Time Analyzer (ClangBuildAnalyzer)

- **Status**: Accepted
- **Date**: 2026-10-16
- **Deciders**: Lusoris Engineering Fleet
- **Tags**: dev, tooling, debug, profiling, docker, ci

## Context

Fleet C/C++ tool evaluation adopted both `rr` for deterministic record-and-replay reverse debugging and `ClangBuildAnalyzer` for compile-time header and template instantiation bottleneck analysis.

Investigating complex video quality pipeline defects, race conditions, memory corruption, and intermittent test regressions across CPU, CUDA, SYCL, and HIP backends requires reproducible execution analysis. Traditional forward debugging in GDB often fails to capture transient Heisenbugs. At the same time, optimizing build performance across the multi-backend C/C++ codebase requires visibility into compiler frontend bottlenecks, which standard build profilers do not provide.

To ensure consistency across developer workstations and CI, both tools must be hermetically provided inside the development container (`dev/Containerfile`), with versions and archive digests pinned directly as `Containerfile` `ARG`s (single source of truth, matching sibling pins such as `ORT_*`, `RUSTUP_INIT_*`, and `PATCHELF_VERSION`) and verified by automated static assertions.

## Decision

We will install `rr` (Ubuntu 26.04 archive package `5.9.0-5`) and `ClangBuildAnalyzer` v1.6.0 (Unlicense, built from source with SHA-256 verification) into the `build-deps` stage of `dev/Containerfile`.

Their versions and digests are pinned directly as `Containerfile` `ARG` defaults (`RR_APT_VERSION=5.9.0-5`, `CLANGBUILDANALYZER_VERSION=1.6.0`, `CLANGBUILDANALYZER_SHA256=868a8d34ecb9b65da4e5874342062a12c081ce4385c7ddd6ce7d557a0c5c292d`). Under HISS-19, ClangBuildAnalyzer tarball download verification is integrated into the existing `scripts/ci/check_ort_tarball_pins.py` (ADR-3334) tracked download table rather than introducing a redundant second checker script or duplicate `build-config.env` pins. The checker enforces:

1. `CLANGBUILDANALYZER_SHA256` is defined in the build block with a valid 64-hex SHA-256 digest.
2. The ClangBuildAnalyzer release tarball is verified via `sha256sum -c` after download and prior to extraction.
3. Tar extraction does not precede verification.

`rr` is installed using `rr=${RR_APT_VERSION}` in the apt layer; like other exact apt pins (`PATCHELF_VERSION`), apt fails the build if the requested archive package version does not resolve.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Separate checker + build-config.env copy | Isolated script dedicated to debug tools | Duplicate pin sources, drift-check plumbing, HISS-19 violation | Rejected: HISS-19 duplicate; generalised `check_ort_tarball_pins.py` table handles all verified tarball downloads with zero second source. |
| Host-only installation | Zero impact on dev container image size | Environmental drift across workstations, manual setup overhead, missing in containerized development | Fails reproducibility and zero-drift environment requirements |
| GDB built-in recording only (`record full`) | Already available with gdb, works without PMU | Records every instruction in software, so runs are far slower and memory-hungry on multi-frame workloads | Inefficient for multi-frame VMAF execution; retained as fallback for PMU-less virtual machines |
| Bloaty / binary size analyzers | High precision for binary footprint inspection | Measures post-link binary size, not compiler frontend/parsing/instantiation time | Does not identify slow compilation headers or template instantiation overhead |

## Consequences

- **Positive**: Developers and automated agents can reproduce Heisenbugs backwards (`rr record` / `rr replay`), use reverse watchpoints, and identify compilation time hotspots with `-ftime-trace` and `ClangBuildAnalyzer`.
- **Negative**: The dev image grows by the `rr` package and its dependencies plus one small `ClangBuildAnalyzer` binary; the ClangBuildAnalyzer build adds a short CMake step to the `build-deps` stage.
- **Neutral / follow-ups**: `rr` needs host `kernel.perf_event_paranoid<=1` and a container without Docker's default seccomp filter (`dev/docker-compose.yml` sets `seccomp=unconfined`; a hand-started container also takes `--cap-add=SYS_PTRACE`, as the rr project advises); environments lacking PMU support fall back to GDB `record full`. Documentation in `docs/development/dev-mcp.md` provides usage instructions.

## References

- Fleet C/C++ diagnostic-tool evaluation, controller order to adopt rr and ClangBuildAnalyzer into the dev image (paraphrased; operator ledger, 2026-10-16).
- [ADR-3334](3334-ort-tarball-sha256-pins.md): tarball pin checker this ADR extends.
- [ADR-0451](0451-local-dev-mcp-container.md): Local dev-MCP container architecture.
- [rr upstream repository](https://github.com/rr-debugger/rr)
- [ClangBuildAnalyzer upstream repository](https://github.com/aras-p/ClangBuildAnalyzer)
