<!-- markdownlint-disable MD060 -->
# VMAFX Deprecations

This file tracks user-visible build configurations and CI modes that have been
deprecated or removed. Entries are ordered newest-first.

---

## 2026-09-18 — 32-bit x86 CI lane (ADR-1258)

**Status**: Removed

The `Ubuntu i686 gcc` lane (formerly `Build — Ubuntu i686 gcc (CPU, no-asm)`),
a compile-only 32-bit cross build with asm disabled, and the `m32` stage of
`scripts/dev/preflight.sh` that mirrored it are gone. ADR-0691 had removed the
lane in May, but a merge restored it the same day, and it never ran a test.

**Migration**: none for users of the published containers or 64-bit builds.
32-bit x86 is unsupported; the fork is 64-bit only. If you build for 32-bit
x86 yourself, add `-msse2 -mfpmath=sse`: without it the x87 FPU's 80-bit
intermediates change scalar scores.

**References**: [ADR-1258](../adr/1258-keep-64-bit-only-retire-i686-lane.md),
[ADR-0691](../adr/0691-vmafx-drop-legacy-build-paths.md)

---

## 2026-05-28 — `ansnr` / `float_ansnr` feature extractor (ADR-0865)

**Status**: Removed

The `ansnr` and `float_ansnr` feature extractors were removed from all backends
(CPU scalar, AVX2, AVX-512, NEON, CUDA, HIP, SYCL, Metal). ANSNR is a legacy
pre-VMAF metric (circa 2001) that Netflix never adopted in any production VMAF
model. Shipped models (such as `vmaf_v0.6.1.json`) do not reference ANSNR, and
empirical feature importance analysis (Research-0733) confirmed zero contribution
to modern VMAF scoring.

**Migration**: Callers requesting distortion-energy metrics should use PSNR
(`psnr_y`, `psnr_cb`, `psnr_cr`) or PSNR-HVS (`psnr_hvs`), both of which remain
actively maintained across CPU and GPU backends. For quality assessment, use
`VmafQualityRunner` with standard models (`vmaf_v0.6.1.json`).

**References**: [ADR-0865](../adr/0865-ansnr-sunset-pre-vmaf-metric-drop.md),
[ADR-0749](../adr/0749-sunset-legacy-vmaf-feature-extractor.md),
[Research-0733](../research/0733-feature-importance-audit-2026-05-28.md),
PR #38

---

## 2026-05-28 — `VmafLegacyQualityRunner` (ADR-0749)

**Status**: Removed (stub retained for import compatibility)

`VmafLegacyQualityRunner` drove the `VmafFeatureExtractor` float-path via the
`vmafexec` binary, collecting the four legacy SVM features (vif, adm, ansnr,
motion) and scoring them with `model_V8a.model`. It became broken when PR #38
dropped the `float_ansnr` feature from the C backend: the runner silently
returned no ansnr scores or raised `KeyError` during SVM scoring.

**Migration**: Replace all uses of `VmafLegacyQualityRunner` with
[`VmafQualityRunner`](../../compat/python-vmaf/core/quality_runner.py) and a
current `.json` model (e.g. `vmaf_v0.6.1.json`) or `vmaf_float_v0.6.1.pkl`.
The class stub is retained in `compat/python-vmaf/core/quality_runner.py` so
existing `import` statements do not raise `ImportError`; instantiation raises
`NotImplementedError` with the same migration pointer.

**References**: [ADR-0749](../adr/0749-sunset-legacy-vmaf-feature-extractor.md),
PR #87

---

## 2026-05-28 — Legacy native build modes (ADR-0728): not carried out

**Status**: Withdrawn. [ADR-0728](../adr/0728-native-build-sunset.md) is
superseded by [ADR-1259](../adr/1259-ci-build-matrix-as-it-runs.md).

This entry used to list 16 CI configurations as removed. None of them was
removed by ADR-0728: its commit changed only this page, the ADR and a changelog
fragment. Of the configurations it listed:

- `Build — Ubuntu i686 gcc (CPU, no-asm)` was removed on 2026-09-18 (see the
  entry above).
- `Build — Ubuntu Vulkan (T5-1b runtime)` and
  `Build — macOS Vulkan via MoltenVK (advisory)` went with the Vulkan backend
  ([ADR-0726](../adr/0726-drop-vulkan-backend.md)).
- Everything else still runs, under the short names it has had since #1286.
  Required: `Windows MinGW64`, `Ubuntu gcc+DNN`, `Ubuntu clang+DNN`,
  `Ubuntu HIP`, `Windows MSVC+SYCL`, `Cppcheck`, `Sanitizers (address)`,
  `Sanitizers (thread)` and `Sanitizers (undefined)`. Not required:
  `macOS clang+DNN`, `macOS Metal`, `Ubuntu gcc static`, `Ubuntu CUDA static`,
  `Ubuntu SYCL` and `Ubuntu SYCL+CUDA`.

`build.yml` (Linux Intel LLVM, macOS Clang+Metal, Windows MSVC+CUDA) runs
alongside `libvmaf-build-matrix.yml`; it did not replace it. A change that
breaks `Windows MinGW64` blocks the merge like any other required check.
ADR-1259 lists every lane and which checks are required.

---

## 2026-05-28 — `python/vmaf/` standalone wheel publishing

**Status**: Confirmed never existed; no-op

The `python/vmaf/` shim package (re-exporting from `compat/python-vmaf/`) was
never published as a standalone wheel to PyPI. No CI job performed wheel
publication for this package at the time of audit. ADR-0691 §4 records this
as a no-op for traceability.

The Netflix Python harness (`python/test/`) continues to run via `tox` in
every Linux lane without a GPU backend and every macOS lane, and the required
`Netflix CPU Golden` job runs the golden assertions.

---
