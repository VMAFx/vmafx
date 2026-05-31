<!-- markdownlint-disable MD013 MD060 -->
# ADR-0603: Ubuntu 26.04 (Resolute Raccoon) fallout fixes — CUDA 13.2, Python 3.14, apt renames

- **Status**: Accepted
- **Date**: 2026-05-19
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `build`, `cuda`, `ci`, `python`, `supply-chain`

## Context

Renovate PR #1402 bumped the dev container base image from `ubuntu:24.04` to
`ubuntu:26.04@sha256:f3d28607…` (codename: Resolute Raccoon). Ubuntu 26.04 ships
several incompatible changes that break the container build:

1. **glibc 2.43 + CUDA 13.0/13.1 `rsqrt` conflict.** Ubuntu 26.04 ships glibc
   2.43 whose `bits/mathcalls.h` declares `rsqrt(double)` with `noexcept(true)`.
   CUDA 13.0 and 13.1's `crt/math_functions.h` redeclares the same symbol
   *without* the noexcept spec, causing NVCC to emit a C2059/C2143/C2085
   cascade during the host-compile pass. CUDA 13.2 aligns its
   `crt/math_functions.h` with glibc 2.43's noexcept annotation and eliminates
   the conflict.

2. **Python 3.14 is the default.** Ubuntu 26.04 (Resolute Raccoon) drops
   Python 3.12 from the archive; `python3` is Python 3.14. The `python3.12`,
   `python3.12-venv`, and `python3.12-dev` packages are not available.
   The `requires-python` ceilings in `tools/vmaf-tune/pyproject.toml`
   (`<3.13`) and `ai/pyproject.toml` (`<3.13`) must be raised to `<3.15`
   to accept 3.14.

3. **`mesa-va-drivers` renamed to `mesa-libgallium`.** Ubuntu 26.04 reorganised
   the Mesa gallium driver packaging; the package formerly known as
   `mesa-va-drivers` (which shipped `radeonsi_drv_video.so` and
   `iris_drv_video.so`) is now `mesa-libgallium`. Requesting the old name
   produces `E: Unable to locate package mesa-va-drivers`.

4. **`libxml2.so.2` soname removed — ROCm LLD broken.** Ubuntu 26.04
   renamed the `libxml2` package to `libxml2-16`, and the shared library
   soname changed from `libxml2.so.2` to `libxml2.so.16`. ROCm 7.2.3's
   bundled LLD (`/opt/rocm-7.2.3/lib/llvm/bin/lld`) was linked against the
   old soname; it fails with `cannot open shared object file: libxml2.so.2`
   at `.hsaco` link time when compiling HIP kernels. The fix installs
   `libxml2-16` and creates a `libxml2.so.2 → libxml2.so.16.1.2` compat
   symlink. The libxml2 C API is backward-compatible; the soname change was
   a Debian packaging decision, not an ABI break.

5. **NVIDIA apt repo.** The NVIDIA `ubuntu2604` apt repository exists and
   provides the `cuda-keyring` .deb and driver packages, but does NOT yet
   contain the full `cuda-toolkit` meta-package (only `cuda-compat-13-2` and
   driver debs are present). The `ubuntu2404` repo carries the full toolkit
   including `cuda-toolkit-13-2`; NVIDIA's .deb packages are
   glibc-backward-compatible and install correctly on Ubuntu 26.04.

6. **ROCm apt channel.** AMD's `repo.radeon.com` only publishes a `noble`
   (Ubuntu 24.04) channel as of 2026-05-19; there is no `resolute`
   (Ubuntu 26.04) channel yet. ROCm userspace .debs are glibc-
   backward-compatible; the `noble` packages install correctly on Ubuntu 26.04.

## Decision

We apply six targeted fixes to the container build and two belt-and-suspenders
defensive additions:

1. **CUDA: pin `cuda-toolkit-13-2` from the ubuntu2404 NVIDIA repo.** Replace
   the unversioned `cuda-toolkit` meta-package with `cuda-toolkit-13-2`. Keep
   the ubuntu2404 CUDA apt repo (the ubuntu2604 repo lacks the full toolkit).
   Bump the CI Jimver/cuda-toolkit-action pin from `13.0.0` to `13.2.0` in
   `libvmaf-build-matrix.yml` (Linux and Windows legs).

2. **CUDA Fix B — `-D__MATH_NO_INLINES` in meson.build.** Add
   `-D__MATH_NO_INLINES` to the nvcc `cuda_flags` in
   `core/src/meson.build`. This flag tells glibc to suppress the inline
   math-function definitions (including the noexcept-annotated `rsqrt` overload),
   eliminating the redeclaration conflict even if a future CUDA patch version
   accidentally re-introduces the mismatch. Harmless on older glibc versions.

3. **Python 3.14: replace package names in Containerfile.** Replace
   `python3.12` / `python3.12-venv` / `python3.12-dev` with
   `python3.14` / `python3.14-venv` / `python3.14-dev`. Use `python3.14 -m
   venv` in the Stage 4 Python environment layer. Raise `requires-python`
   ceilings in `tools/vmaf-tune/pyproject.toml` and `ai/pyproject.toml` from
   `<3.13` to `<3.15`. Python 3.14 wheels for every declared dependency
   (numpy 2.4.5, scipy, torch, etc.) are available on PyPI.

4. **`mesa-va-drivers` → `mesa-libgallium`.** Replace the old package name in
   the Containerfile Stage 1 apt install list. Update the post-install
   verification `RUN` to check `mesa-libgallium`.

5. **ROCm: document the `noble`-only situation.** No change to the ROCm apt
   source line (it already uses `noble`). Add a comment explaining that
   `resolute` does not exist yet and when to flip it.

6. **`libxml2.so.2` compat symlink for ROCm LLD.** Install `libxml2-16`
   (Ubuntu 26.04's renamed libxml2 package) and create a
   `libxml2.so.2 → libxml2.so.16.1.2` symlink so that ROCm 7.2.3's
   bundled LLD can load the library at `.hsaco` link time. The symlink
   is added in the same `RUN` step as the `rocm-hip-runtime-dev` install
   to keep layer count unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Pin back to CUDA 13.1 + add `-D__MATH_NO_INLINES` only | Avoids NVIDIA repo complexity | CUDA 13.1 does not exist in the ubuntu2404 apt repo; unversioned `cuda-toolkit` meta-package would resolve to 13.2 anyway; also no ubuntu2604 toolkit | Only `cuda-toolkit-13-2` is available in the NVIDIA apt repos |
| Stay on ubuntu:24.04 base image | No compatibility work needed | Renovate PR #1402 already bumped it; ubuntu:24.04 hits EOL in 2029; ubuntu:26.04 is the LTS target | Ubuntu 26.04 is the new LTS baseline; reverting Renovate is the wrong direction |
| Use ubuntu2604 NVIDIA repo for keyring and toolkit | Keeps URLs consistent | ubuntu2604 repo only has `cuda-compat-13-2` + driver packages — no `cuda-toolkit` meta-package | ubuntu2404 toolkit cross-installs cleanly and is the only path to the full nvcc on 26.04 |
| Wait for ROCm `resolute` channel | Cleanest repo URL | AMD has not published it; container build would fail indefinitely | `noble` packages install on resolute; `noble` channel is the pragmatic fix |
| Install Ubuntu system `lld` to replace ROCm's bundled one | Avoids soname shim | System LLD may not support `amdgcn` target; ROCm LLD is patched with AMD-specific backends not in upstream LLVM | Symlink is the minimal invasive fix that leaves ROCm's toolchain intact |

## Consequences

- **Positive**: container build unblocked on Ubuntu 26.04 LTS; CUDA 13.2 fixes
  the glibc 2.43 `rsqrt` noexcept conflict permanently; Python 3.14 support
  opens doors to CPython 3.14 free-threaded builds in future iterations; HIP
  kernel compilation restored by the `libxml2.so.2` compat symlink.
- **Negative**: `cuda-toolkit` is now pinned to a versioned meta-package
  (`cuda-toolkit-13-2`); future CUDA major releases require an explicit Containerfile
  bump rather than being pulled in automatically by the unversioned meta.
- **Neutral / follow-ups**:
  - When NVIDIA publishes `cuda-toolkit-13-3` or later: update the
    `cuda-toolkit-13-2` pin. The `-D__MATH_NO_INLINES` flag is retained as a
    permanent defensive measure.
  - When AMD publishes a `resolute` ROCm channel: replace `noble` with
    `resolute` in the ROCm apt source line.
  - When Ubuntu 26.04 builds a `python3.12` backport (unlikely for LTS):
    no action required; `python3.14` is the correct target.
  - CI gate `Build — Ubuntu CUDA` and `Build — Windows MSVC + CUDA` now test
    CUDA 13.2 rather than 13.0. The sm_50 gencode guard (`<13` version check
    in `meson.build`) is unaffected — 13.2 is still ≥ 13.

## References

- Renovate PR #1402 — triggered the Ubuntu 26.04 bump.
- [ADR-0541](0541-dev-mcp-container-full-gpu-stack.md) — original container
  GPU stack design (CUDA, SYCL, Vulkan, ROCm, oneAPI).
- [ADR-0568](0568-sdk-audit-2026-05-18.md) — SDK audit that introduced ORT
  1.26.0, vvenc 1.14.0, AMF 1.5.2 (same PR round).
- NVIDIA ubuntu2604 apt repo index: `https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2604/x86_64/`
- ROCm apt repo: `https://repo.radeon.com/rocm/apt/7.2.3/dists/` (noble only as of 2026-05-19).
- Source: req — "fix every fallout in ONE DRAFT PR" (user directive, 2026-05-19).
