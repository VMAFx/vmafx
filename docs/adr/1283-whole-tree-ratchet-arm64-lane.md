<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1283: The whole-tree clang-tidy ratchet gets an arm64 cross lane

- **Status**: Proposed
- **Date**: 2026-09-21
- **Deciders**: Lusoris
- **Tags**: ci, clang-tidy, lint, ratchet, simd, arm64, fork-local

## Context

[ADR-1142](1142-whole-codebase-standards.md) put every file in the tree under the
same coding standards and bounded the tree with a clang-tidy ratchet:
`scripts/ci/tidy-ratchet.py` measures every translation unit of a
`compile_commands.json` and compares the per-file numbers against
`scripts/ci/tidy-baseline-<lane>.json`. Four lanes existed — `cpu`, `cuda`, `hip`
and `sycl` — and **none of them compiles AArch64**.

The consequence is that the entire NEON and SVE2 tree was outside the bound the
rule claims to place on the whole tree:

- **32 translation units exist that no x86 build compiles at all**, so no x86
  lane's compile database holds a command for any of them. Diffing the
  `measured_sources` of the two baselines names them: the 20 sources under
  `core/src/feature/arm64/` (`core/src/meson.build` builds the `arm64_v8` /
  `arm64_v8_fp` / `arm64_ssim_neon` / `arm64_ssimulacra2` /
  `arm64_adm_dwt2_neon` / SVE2 static libraries only when
  `host_machine.cpu_family()` is `aarch64`), `core/src/arm/cpu.c` (the AArch64
  runtime feature detection), and the 11 dedicated NEON parity tests in
  `core/test/` — `test_vif_neon.c`, `test_ciede_neon.c`, `test_ssim_neon.c`,
  `test_psnr_neon.c`, `test_psnr_hvs_neon.c`, `test_motion_neon.c`,
  `test_motion_pipeline_neon.c`, `test_adm_dwt2_neon.c`,
  `test_float_adm_neon.c`, `test_float_adm_dwt2_neon.c`,
  `test_float_motion_neon.c` — whose meson executables are gated on
  `['aarch64', 'arm64']`.
- On top of those, the `ARCH_AARCH64`-guarded halves of the shared SIMD parity
  tests (`test_vif_simd.c`, `test_cambi_simd.c`, …) are preprocessed away in the
  `cpu` lane, so those *files* are measured while their AArch64 code is not.
- The fast `Tidy Changed` job in `.github/workflows/lint-and-format.yml` excludes
  `^core/src/feature/arm64/` outright in `exclude_untidyable()`, for the correct
  local reason that its CPU-only `build/` has no compile command for those files.
  So a PR that edits a NEON kernel got no clang-tidy from that job either.

That is not a theoretical gap. `core/src/feature/arm64/vif_neon.c` carried **36
clang-tidy findings** that nothing in CI had ever reported; they surfaced only
when someone ran clang-tidy against a hand-made aarch64 cross build
(`T-NO-ASM-SIMD-TEST-WARNINGS-2026-09-18`, `docs/state.md`). The rebase note for
that work records the gap in one line: *"No CI lane measures the arm64 tree, so
nothing else catches a regression here."*

## Decision

We will add an `arm64` lane to the ADR-1142 ratchet, defined the same way the
existing lanes are: a build directory configured for the lane, a
`TIDY_RATCHET_EXTRA_arm64` argument set in the `Makefile`, and a measured
`scripts/ci/tidy-baseline-arm64.json`.

The lane cross-compiles. Its compile database comes from the in-tree cross file
`build-aux/aarch64-linux-gnu.ini` (`aarch64-linux-gnu-gcc`), and clang-tidy is
given the matching `--target` and `--sysroot` so it parses those commands as
AArch64:

```bash
meson setup build-arm64 core --cross-file build-aux/aarch64-linux-gnu.ini \
    -Denable_cuda=false -Denable_sycl=false -Db_lto=false
make tidy-ratchet LANE=arm64 TIDY_RATCHET_BUILD_DIR=build-arm64
```

Without `--target=aarch64-linux-gnu` clang-tidy reads `<arm_neon.h>` and
`<arm_sve.h>` against the host's x86 headers and every NEON translation unit is a
`clang-diagnostic-error`; without `--sysroot` it resolves libc against the host's
headers. Both values are the cross package's defaults and are overridable
(`AARCH64_TARGET`, `AARCH64_SYSROOT`).

The lane is at **parity with `cuda`, `hip` and `sycl`**: measured, committed,
runnable locally, and *not* wired as a PR-required CI context by this ADR.
Promoting any non-`cpu` lane to a required context is a separate CI-gate decision,
and it additionally requires the baseline to be re-recorded from the CI image's
own measurement, because the ratchet's counts depend on the C compiler's system
headers as much as on clang-tidy's version (ADR-1230).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Cross-build lane over the full aarch64 compile database** (chosen) | Defined exactly like the other four lanes; measures the AArch64 bodies of `core/test/` too, not only `core/src/feature/arm64/`; no new tooling | Its ~270 non-AArch64 TUs overlap the `cpu` lane under a different compiler, so unrelated `cpu` cleanups make the `arm64` baseline stale-high | — |
| Narrow lane measuring only the 20 `core/src/feature/arm64/` TUs | No overlap with the `cpu` lane, cheapest to run | Needs a synthesised or filtered compile database (a second mechanism next to `gen-sycl-compile-commands.py`); `--only` exists for scoped *tightening* of an existing baseline (ADR-1243), not for defining one; and it would still leave the `ARCH_AARCH64` test bodies unmeasured — the same defect one directory over | Rejected: solves the filed symptom, not the class |
| Native aarch64 Linux runner | Measures what actually ships; no sysroot or target flags | This repo configures no hosted or self-hosted Linux aarch64 runner — the only native ARM64 runner is `windows-11-vs2026-arm`, which is MSVC and has no clang-tidy | Rejected: cannot be built today; the cross lane is what is available now |
| A second, clang-based cross file (`aarch64-linux-gnu-clang.ini`) so the compile commands carry `--target` themselves | clang-tidy needs no extra arguments | A second toolchain file for a target the tree already has one for, diverging from every arm64 build, test and CI path that uses `build-aux/aarch64-linux-gnu.ini` | Rejected: the two `--extra-arg` values go through `TIDY_RATCHET_EXTRA_<lane>`, the mechanism the `cuda` and `hip` lanes already use for exactly this |
| Leave `core/src/feature/arm64/` in `exclude_untidyable()` and do nothing else | No work | That exclusion *is* the defect; ADR-1142 §1 removed the tiers that would have justified it | Rejected |

## Consequences

- **Positive**: the NEON and SVE2 tree is measured and bounded for the first
  time. A regression in `core/src/feature/arm64/` now has a number that can move,
  and `make tidy-ratchet LANE=arm64` is a command a rebase or SIMD PR can run.
  The AArch64 bodies of the SIMD parity tests come along with it.
- **Positive**: the lane needs no GPU and no proprietary SDK — a cross gcc and a
  glibc sysroot are two apt/pacman packages, so unlike the `cuda`, `hip` and
  `sycl` lanes it is reproducible on a hosted x86 runner.
- **Negative**: the lane's baseline is recorded on a workstation toolchain, so it
  is not directly comparable to a measurement taken elsewhere. The ratchet
  annotates a `cc_version` / `clang_tidy_version` mismatch, but a differing
  toolchain will still read as a per-file delta. Any promotion to CI re-records
  the baseline from CI's own run (ADR-1230, and the `cpu` lane's precedent).
- **Negative**: because the compile database covers the whole aarch64 build, a
  `cpu`-lane cleanup in a shared file leaves the `arm64` baseline stale-high
  until someone re-measures it. This is already true of `cuda`, `hip` and `sycl`.
- **Neutral / follow-up**: `exclude_untidyable()` keeps its
  `^core/src/feature/arm64/` entry — the fast `Tidy Changed` job still has no
  compile command for those files — but its comment now names the lane that does
  measure them, the same way the CUDA and HIP entries name `make tidy-ratchet
  LANE=cuda` / `LANE=hip`.
- **Neutral / follow-up**: whether the non-`cpu` lanes become required contexts,
  or run in `nightly.yml`, remains open and is deliberately not decided here.

## References

- [ADR-1142](1142-whole-codebase-standards.md) — whole-codebase standards and the
  ratchet this lane joins.
- [ADR-1230](1230-modern-gcc-toolchain.md) — a ratchet baseline depends on the C
  compiler's system headers, so it is re-recorded from the toolchain that gates it.
- [ADR-1243](1243-tidy-scoped-baseline-tightening.md) — `--only … --write`
  tightens an existing baseline; it does not define one.
- [ADR-1260](1260-windows-arm64-cpu-lane.md) — the other AArch64 lane in CI
  (MSVC, build + test, no clang-tidy).
- `docs/state.md`, `T-NO-ASM-SIMD-TEST-WARNINGS-2026-09-18` — the 36 unmeasured
  `vif_neon.c` findings that motivated this.
- Source: `req` — paraphrased user direction: fix the tracked bug that no
  clang-tidy lane measures the arm64 sources, and do not fake a lane that
  measures nothing.
