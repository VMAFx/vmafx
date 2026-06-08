<!-- markdownlint-disable MD060 -->
# VMAFX Deprecations

This file tracks user-visible build configurations and CI modes that have been
deprecated or removed. Entries are ordered newest-first.

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

## 2026-05-28 — Legacy native build modes (ADR-0728)

**Status**: Removed

**What was removed**: The following CI build configurations no longer run as
required or advisory CI lanes:

| Removed configuration | Last active |
|---|---|
| `Build — Windows MinGW64 (CPU)` (MSYS2 / MinGW-w64) | pre-ADR-0728 |
| `Build — Ubuntu i686 gcc (CPU, no-asm)` (32-bit x86 cross-build) | pre-ADR-0728 |
| `Build — Ubuntu gcc (CPU) + DNN` | pre-ADR-0728 |
| `Build — Ubuntu clang (CPU) + DNN` | pre-ADR-0728 |
| `Build — macOS clang (CPU) + DNN` | pre-ADR-0728 |
| `Build — Ubuntu Vulkan (T5-1b runtime)` | pre-ADR-0728 |
| `Build — macOS Vulkan via MoltenVK (advisory)` | pre-ADR-0728 |
| `Build — Ubuntu HIP (T7-10b runtime)` | pre-ADR-0728 |
| `Build — macOS Metal (T8-1 scaffold)` | pre-ADR-0728 |
| `Build — Ubuntu gcc Static (CPU)` | pre-ADR-0728 |
| `Build — Ubuntu CUDA Static` | pre-ADR-0728 |
| `Build — Ubuntu SYCL` | pre-ADR-0728 |
| `Build — Ubuntu SYCL + CUDA` | pre-ADR-0728 |
| `Build — Windows MSVC + oneAPI SYCL (build only)` | pre-ADR-0728 |
| `Cppcheck (Whole Project)` (required-check) | pre-ADR-0728 |
| `Sanitizers — ASan + UBSan + MSan (address/thread/undefined)` (3 separate required-checks) | pre-ADR-0728 |

**Why**: Container-first VMAFX posture (ADR-0686, ADR-0701). Docker images and
the Helm chart are the user-facing artifacts; per-backend host binaries are no
longer published. The canonical build matrix is now `build.yml` (ADR-0710):
one job per OS (Linux GCC all-backends, macOS Clang, Windows MSVC+CUDA).

**Migration**: No migration needed for end users — the C library API and CLI
flags are unchanged. Contributors who previously tested against MinGW64 should
use the Windows MSVC + CUDA path or the `vmafx-dev-mcp` Docker container.
32-bit x86 is unsupported; the fork is 64-bit only.

**References**: ADR-0691, ADR-0710, ADR-0728

---

## 2026-05-28 — `python/vmaf/` standalone wheel publishing

**Status**: Confirmed never existed; no-op

The `python/vmaf/` shim package (re-exporting from `compat/python-vmaf/`) was
never published as a standalone wheel to PyPI. No CI job performed wheel
publication for this package at the time of audit. ADR-0691 §4 records this
as a no-op for traceability.

The Netflix Python harness (`python/test/`) continues to run via `tox` in the
Linux full-build leg.

---
