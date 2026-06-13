<!-- markdownlint-disable MD013 MD060 -->
# ADR-0027: Non-conservative image pins with experimental toolchain flags

- **Status**: Accepted
- **Date**: 2026-04-17
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, cuda, sycl, build, supply-chain

## Context

The fork's container base images and toolchain pins had been conservative (N-2 style) by default. The user directed them to track the newest stable — especially CUDA — and to enable "experimental features" where they unlock measurable throughput. A subsequent clarification narrowed "experimental" to feature flags on stable compilers, not preview compilers themselves: "with experimental i meant features not version so it should be >13.2 on cuda".

## Decision

Container base images and toolchain pins track the latest stable release. Specifically: CUDA base tracks NVIDIA's newest `devel-ubuntu24.04` tag (2026-04-17: `13.2.0`; bump on every stable release); Ubuntu 24.04 LTS is the floor; FFmpeg uses the current `n<major>.<minor>` tag; oneAPI uses the current major.minor. Dev and prod Dockerfiles track the same major.minor unless there's a hard blocker. Enable experimental toolchain feature flags where they give measurable throughput or unblock modern C++: nvcc `--expt-relaxed-constexpr`, `--extended-lambda`, `--expt-extended-lambda`; SYCL/DPC++ `-fsycl-unnamed-lambda`, `-fsycl-allow-func-ptr`, `-fsycl-device-code-split=per_kernel`; Blackwell gencode (sm_120). Not preview/beta CUDA branches. Digest pins retained for reproducibility on the pinned release, but bumped aggressively on every stable release rather than N-2.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Conservative N-2 pins | Maximum stability | Leaves speed on the table; user explicitly rejected | Rejected |
| Preview CUDA branches | Newest features | Sacrifices reproducibility without matching speed win | Rejected per user clarification |
| Latest stable + feature flags (chosen) | GPU perf without instability | Must track NVIDIA release cadence | Matches user's clarified scope |

Rationale note: the user's two messages combined — "don't make the img pins conservative... use newer versions... especially cuda and even use experimental features (thats where the speed is)" and the clarification about "features not version". So "experimental" = toolchain feature flags on stable CUDA ≥13.2 / stable DPC++.

## Consequences

- **Positive**: GPU builds get Blackwell gencode and relaxed-constexpr for free; container tracks current CUDA.
- **Negative**: monthly-to-quarterly bump cadence; CI must re-validate on every bump.
- **Neutral / follow-ups**: commit `8a995cb0` tracks the 13.2 bump.

## References

- Source: `req` (user: "dont make the img pins conservative... use newer versions... especially cuda and even use experimental features" + "with experimental i meant features not version so it should be >13.2 on cuda")
- Related ADRs: ADR-0022
