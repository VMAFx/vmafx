<!-- markdownlint-disable MD060 -->
# ADR-0407: AdaptiveCpp as a second SYCL toolchain

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: sycl, build, toolchain, fork-local, ci, contributor-experience

## Context

The fork's `-Denable_sycl=true` build path has been hard-wired to
Intel's `icpx` compiler (oneAPI DPC++) since the SYCL backend landed
([ADR-0027](0027-non-conservative-image-pins.md),
[ADR-0217](0217-sycl-toolchain-cleanup.md)). The full Intel oneAPI
Base Toolkit installer is ~2.6 GB, ships closed-source binaries, and
is awkward to obtain on hosts that do not have Intel hardware to
target. This is a real friction point for new contributors who only
need to type-check or run unit tests against the SYCL TUs — the bar
is currently "install all of oneAPI", even when the contributor
machine has no Intel iGPU, no Arc GPU, and no NPU.

Topic B of [Research-0086](../research/0086-sycl-toolchain-audit-2026-05-08.md)
recommended **GO-AS-SECOND-TOOLCHAIN** for AdaptiveCpp (formerly
OpenSYCL / hipSYCL). AdaptiveCpp is a community-driven, BSL-licensed
SYCL implementation built on LLVM. It supports CUDA, HIP, OpenMP CPU,
and a generic SPIR-V backend, and it is the only realistic
open-source SYCL implementation in 2026.

The audit identified two showstoppers: (1) ten kernel sites use the
Intel-specific `[[intel::reqd_sub_group_size(32)]]` attribute (and
one uses `SG_SIZE` parameterised) that AdaptiveCpp rejects; (2) the
`core/src/meson.build` SYCL block hard-codes icpx-only flags
(`-fsycl`, `-fp-model=precise`) and Intel runtime libraries (`svml`,
`irc`). Both are mechanical to wrap.

## Decision

**Add AdaptiveCpp as a second supported SYCL toolchain. Intel oneAPI
`icpx` remains the primary toolchain.**

Concretely:

1. **`core/src/feature/sycl/sycl_compat.h`** (new, ~60 LOC). One
   public macro: `VMAF_SYCL_REQD_SG_SIZE(N)`. Expands to the Intel
   attribute under `__INTEL_LLVM_COMPILER`, to a no-op under
   `SYCL_IMPLEMENTATION_ACPP` / `SYCL_IMPLEMENTATION_HIPSYCL`. The
   ten previously hard-coded call sites switch to the macro.
2. **`core/src/meson.build`** detects the configured
   `sycl_compiler` by basename. `acpp` / `syclcc` / `syclcc-clang`
   take the AdaptiveCpp path: `--acpp-targets=<value>` instead of
   `-fsycl`, `-ffp-contract=off` instead of `-fp-model=precise`,
   and `libacpp-rt.so` (with `libhipSYCL-rt.so` legacy fallback) as
   the runtime library. Intel `svml` / `irc` are skipped under
   AdaptiveCpp.
3. **`core/meson_options.txt`** adds `sycl_acpp_targets` (default
   `generic`) so contributors can override the AdaptiveCpp target
   list (e.g. `omp;cuda:sm_75`). `sycl_compiler` description is
   updated to advertise both toolchains.
4. **`docs/development/sycl-toolchains.md`** (new) documents the
   AdaptiveCpp install path (Arch AUR `adaptivecpp`, source build),
   the capability matrix, the numerical-conformance gap vs icpx, and
   the supported `--acpp-targets` values.

Numerical conformance. `--acpp-targets=omp` (CPU OpenMP) is **not**
bit-identical to `icpx` and **not** bit-identical to scalar CPU; the
fork's
[`feedback_golden_gate_cpu_only`](../../README.md#netflix-golden-data-gate)
rule already documents that no GPU/SYCL backend is bit-identical to
scalar CPU. The AdaptiveCpp lane is a "yet another non-bit-identical
backend" with its own ULP-tolerance budget when the cross-backend
gate is extended to cover it (deferred to a follow-up PR; this PR
ships the build plumbing only).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Stay icpx-only | Zero new code; one supported path; bit-identical-to-icpx is the only acceptance bar. | 2.6 GB closed-source toolchain remains a contributor blocker; CI runners without Intel hardware cannot exercise SYCL TUs. | Solves the wrong problem — Research-0086 Topic B specifically flagged contributor friction as a real cost. |
| Replace icpx with AdaptiveCpp | One toolchain, open source, smaller install. | Loses Intel discrete-GPU codegen quality; OpenVINO / NPU enablement story is icpx-coupled; published-binary ABI changes. | Net regression for the fork's primary user — Intel hardware. icpx stays primary. |
| Wrap differences in a pure CMake/meson "compatibility shim" without source-level macros | No source churn. | Pre-processor branch logic still has to live somewhere; `[[intel::reqd_sub_group_size]]` cannot be hidden behind a meson flag because it appears in kernel-lambda attribute position. | Source-level macro is the minimal-surface fix. |
| Drop the `reqd_sub_group_size` attribute entirely under both toolchains | One code path, no compat header. | The attribute is load-bearing for icpx codegen quality on Arc / Battlemage; removing it loses a measured ~5–8 % on `motion_v2_sycl` per the T7-8 bench audit. | Trades the wrong cost — keep the attribute for icpx, neutralise it for acpp. |

## Consequences

**Positive:**

- Contributors without Intel hardware can install AdaptiveCpp from
  the AUR (or build from source) and get a working
  `-Denable_sycl=true` build.
- A future CI lane can run SYCL unit tests on stock
  `ubuntu-latest` runners via `--acpp-targets=omp`, catching
  toolchain-monoculture bugs that only one vendor would otherwise
  hide.
- The `sycl_compat.h` shim is a useful lever: any future Intel- or
  vendor-specific SYCL extension (e.g. the `group_load` rewrite
  proposed in Research-0086 Topic A.4) lands behind a macro
  symmetric to `VMAF_SYCL_REQD_SG_SIZE`.

**Negative:**

- One more toolchain to support means one more CI lane's worth of
  drift to audit on each rebase; the new ULP-tolerance budget for
  acpp's CPU OpenMP backend is yet to be measured.
- Sub-optimal sub-group selection on Intel HW under AdaptiveCpp:
  the runtime picks the "natural" sub-group size per backend, which
  is not always 32 on Xe-HPG / Xe-HPC; throughput on Intel devices
  under acpp may trail icpx output by a few percent on the affected
  kernels.

**Neutral / follow-ups:**

- A separate PR can add `.github/workflows/sycl-acpp.yml` (sized
  ~50 LOC in Research-0086 Topic B.4) that builds with AdaptiveCpp
  on a stock `ubuntu-latest` runner.
- `make lint` / `clang-tidy` have no AdaptiveCpp-specific lane in
  this PR; the existing `scripts/ci/clang-tidy-sycl.sh` wrapper
  ([ADR-0217](0217-sycl-toolchain-cleanup.md)) targets icpx
  paths and is unaffected.
- The Netflix golden gate (CPU-only,
  [CLAUDE.md §8](../../CLAUDE.md#8-netflix-golden-data-gate-do-not-modify))
  is unaffected — it does not exercise the SYCL backend.
- ADR-0217 gets a `### Status update 2026-05-08: AdaptiveCpp added`
  appendix per the ADR-0028 maintenance rule.

## References

- [Research-0086 §Topic B](../research/0086-sycl-toolchain-audit-2026-05-08.md)
  — the SYCL toolchain audit that proposed this work.
- [ADR-0027](0027-non-conservative-image-pins.md) — current SYCL
  experimental flags, all icpx-coupled.
- [ADR-0217](0217-sycl-toolchain-cleanup.md) — multi-version oneAPI
  recipe + clang-tidy wrapper.
- [ADR-0220](0220-sycl-fp64-fallback.md) — fp64-free kernel
  contract (preserved verbatim under both toolchains).
- [`core/src/sycl/AGENTS.md`](../../core/src/sycl/AGENTS.md)
  — SYCL invariants on rebase.
- AdaptiveCpp upstream: <https://adaptivecpp.github.io/AdaptiveCpp/>
  (compiler identification macros, target syntax).
- AUR `adaptivecpp` 25.10.0-2 — the pinned reference for Arch
  contributors.
- `req` — user direction 2026-05-08: "GO-AS-SECOND-TOOLCHAIN
  recommendation in the digest".
