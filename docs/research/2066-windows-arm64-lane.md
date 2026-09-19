<!-- markdownlint-disable MD013 -->

# 2066 — Windows on ARM64: what the runner offers, which toolchain, what broke

**Date**: 2026-09-19
**Scope**: adding a native Windows-on-ARM64 (WoA) CPU build-and-test lane
before `1.0.0-rc.1` ([ADR-1260](../adr/1260-windows-arm64-cpu-lane.md)).
**Outcome**: `Windows ARM64 MSVC` job on `windows-11-vs2026-arm`, MSVC
ARM64-hosted toolset, meson `fast` suite; two source-tree fixes.

## Why now

The maintainer flagged WoA as a pre-rc1 to-do after CUDA 13.4 added
`windows-arm64` packages. NVIDIA's `redistrib_13.4.2.json` lists
`windows-arm64` for `cuda_nvcc` and `cuda_cudart` in 13.4.1 (2026-09-09) and
13.4.2 (2026-09-16); 13.3.1, the pin used by every CUDA lane and by
`build-config.env`, has no ARM64 Windows packages. The CUDA bump is blocked on
`Jimver/cuda-toolkit` PR #448, so the CUDA-on-WoA leg is a follow-up; the CPU
lane is what this digest covers.

## What no lane covered

`git grep` over `.github/workflows/` finds no Windows ARM runner label. The
AArch64 tree is compiled by the `Ubuntu ARM clang` lane (`ubuntu-24.04-arm`,
clang 22) and the macOS lanes (Apple clang). The three Windows lanes build
x86 only. So the following had never been compiled by `cl.exe`:

- `core/src/feature/arm64/*.c` (20 NEON TUs, 2 SVE2 TUs),
- every `#if ARCH_AARCH64` branch in `core/src/feature/*.c` (`integer_adm.c`
  includes `<arm_neon.h>` directly, `cambi.c`, `ciede.c`, `float_*.c`,
  `integer_*.c`, `ms_ssim_decimate.c`, `ssimulacra2.c`),
- `core/src/arm/cpu.c`,
- the AArch64-gated tests in `core/test/meson.build` (`psnr_hvs_neon_test_archs`
  and the `['x86_64', 'x86', 'aarch64', 'arm64']` lists).

ADR-1040 (2026-06-04) says "macOS arm64 (`macos-15-arm64`) and Windows arm64
CI build cleanly"; the Windows half of that sentence had no lane behind it.

## The runner, verified live

Source: `actions/runner-images` `README.md` and the two image READMEs
(`images/windows/Windows11-Arm64-Readme.md`,
`images/windows/Windows11-VS2026-Arm64-Readme.md`), read through the GitHub
API on 2026-09-19; docs.github.com "GitHub-hosted runners reference".

| Label | Image | Visual Studio | Notes |
| --- | --- | --- | --- |
| `windows-11-arm` | Windows 11 Arm64, 20260906.161.1 | Enterprise 2022 17.14.37614.0 | migrating to the VS 2026 image 2026-09-21 to 2026-09-30 (issue #14602) |
| `windows-11-vs2026-arm` | Windows 11 Arm64 with VS 2026, 20260907.151.1 | Enterprise 2026 18.9.12120.119 | GA since 2026-08-19 (issue #14592) |

Both: free and unlimited for public repositories (4 vCPU, 16 GB RAM, 14 GB
SSD, arm64), `Microsoft.VisualStudio.Component.VC.Tools.ARM64` (and ARM64EC),
`VC.Llvm.Clang` / `VC.Llvm.ClangToolset`, Python 3.13.15 with pip 26.2.1,
Ninja 1.13.2, CMake 4.4.3, LLVM 22.1.8, Git 2.55, MSYS2 (installed, not on
PATH), vcpkg. Neither ships meson or nasm.

The x64 Windows lanes pin `windows-2025`; the README now maps that label to
the `windows-2025-vs2026` image, so all MSVC lanes are on Visual Studio 2026.

Decision: `windows-11-vs2026-arm`, because the other label may land on either
image during the migration window that overlaps this PR's CI iterations.

## The toolchain

Candidates on the image: MSVC ARM64-hosted (`cl.exe`), clang-cl (LLVM 22, and
the VS-bundled Clang toolset), and MSYS2's llvm-mingw or clang. The existing
Windows MSVC legs (`windows-gpu-build`, `build.yml`) use
`TheMrMilchmann/setup-msvc-dev@v4.1.0` with `arch: x64`, `cl.exe`, and
`/experimental:c11atomics` in `CFLAGS`/`CXXFLAGS`. The lane mirrors them.

How `arch: arm64` reaches the compiler: `src/setup-msvc-dev.ts` runs
`"${vcvarsallPath}" ${arch}` after `normalizeArch`, which only rewrites the
x86/x64 spellings and passes everything else through. The action's README
lists `x64`, `x86` and the cross triplets only; the msvc-170 documentation
table lists `x86_arm64` and `amd64_arm64`. The ARM64-hosted native toolset,
`VC\Auxiliary\Build\vcvarsarm64.bat`, ships since Visual Studio 2022 17.4 and
`vcvarsall.bat arm64` selects it. Evidence beyond memory: a GitHub code
search for workflows on `windows-11-arm` that give `arch: arm64` to
`ilammy/msvc-dev-cmd` (the action `setup-msvc-dev` is the Node 24 port of)
returns 403 files, among them `chriskohlhoff/asio` `.github/workflows/windows.yml`
and `telegramdesktop/tdesktop` `.github/workflows/win.yml`; with
`setup-msvc-dev` itself, 34 files (`radareorg/radare2`, `LMMS/lmms`). The
lane also runs `cl.exe 2>&1 | findstr /i /C:"for ARM64"` before configuring,
so a wrong host toolset fails the job in seconds rather than after a build.

Python: `actions/setup-python` 3.14.7 works on the runner because
`actions/python-versions` `versions-manifest.json` has `win32`/`arm64` builds
for 3.14.7 (and 3.13.15). `pip install meson ninja` works because PyPI's
`ninja` 1.13.2 ships `ninja-1.13.2-py3-none-win_arm64.whl`; the image's own
Ninja 1.13.2 is the fallback.

nasm: `core/src/meson.build` probes NASM only inside
`if host_machine.cpu_family().startswith('x86')`, and the `x86/cpuid.asm`
object is added under the same condition, so an ARM64 configure never asks
for it.

meson's view of the host: `mesonbuild/envconfig.py` (meson 1.12.0) maps
`platform.machine() == 'arm64'` to `cpu_family() == 'aarch64'`. Every
AArch64 gate in `core/src/meson.build` and `core/test/meson.build` tests
`cpu_family().startswith('aarch64')` or a list containing `'aarch64'`, so
the NEON TUs and the NEON tests build on Windows ARM64 without a meson
change.

## What broke, by reading the tree

Grep sweep over `core/src/feature/arm64/`, `core/src/arm/`, the
`ARCH_AARCH64` branches and the AArch64-gated tests for constructs MSVC does
not accept (`__attribute__`, `__builtin_*`, `#pragma GCC`, GCC vector
extensions on NEON types, brace-initialised or compound-literal vectors,
`_x2`/`_x3`/`_x4` multi-register loads, `restrict`, POSIX headers):

1. `core/test/test_ciede_neon.c`: `<sys/mman.h>`, `<unistd.h>`,
   `sigaction`, `sigsetjmp`/`siglongjmp`, `sysconf(_SC_PAGESIZE)`, `_exit`
   under `#if ARCH_AARCH64`. Compile error on MSVC. Fixed with a platform
   layer: POSIX keeps `mmap` + `mprotect(PROT_NONE)` + `sigsetjmp`; Windows
   uses `VirtualAlloc(MEM_RESERVE | MEM_COMMIT)`,
   `VirtualProtect(PAGE_NOACCESS)` and SEH `__try` /
   `__except (EXCEPTION_ACCESS_VIOLATION)`, page size from
   `GetSystemInfo`. Non-SEH Windows compilers hit an `#error`; there is no
   such compiler for AArch64 Windows in practice. The rewrite removed the
   file's two `goto`s and split `probe_overread` (75 lines) into
   `probe_outputs_alloc`, `probe_slack`, `run_kernel_guarded` and the sweep,
   all under 60 lines. Verified on the Linux side with the
   `~/.cache/vmafx-cross/aarch64-clang.ini` cross file under
   `qemu-aarch64-static`: `meson test -C build/aarch64 test_ciede_neon`
   passes 4 of 4 checks before and after, 0 compiler warnings.
2. `core/src/meson.build`: the four float NEON carve-outs
   (`arm64_v8_fp`, `arm64_adm_dwt2_neon`, `arm64_ssim_neon`,
   `arm64_ssimulacra2`) and the two SVE2 libraries passed
   `-ffp-contract=off` (and `-march=armv9-a+sve2`) unconditionally. `cl.exe`
   emits `D9002` and ignores them. Microsoft Learn, "/fp (Specify
   floating-point behavior)": under `/fp:precise`, the default,
   "floating-point contractions aren't generated by default. This behavior
   is new in Visual Studio 2022", and `/fp:contract` is what turns them on.
   Fixed with `arm64_strict_fp_args`: `['/fp:precise']` on `msvc`,
   `['-ffp-contract=off']` otherwise; the SVE2 `cc.compiles()` probe is
   skipped on `msvc` (no `<arm_sve.h>`, so it could only fail after a
   warning). clang-cl is deliberately not covered: it accepts `/fp:precise`
   but keeps clang's in-statement contraction, so it would need
   `/clang:-ffp-contract=off`; no clang-cl lane exists.
3. Inert on MSVC and left as they are: `#pragma GCC diagnostic push/pop`
   around `#pragma STDC FP_CONTRACT OFF` in five NEON TUs and the
   `__attribute__((optimize("-ffp-contract=off")))` in
   `float_adm_dwt2_neon.c` are all under `#if defined(__GNUC__) && !defined(__clang__)`;
   `compat_builtin.h` already defines `__builtin_clz`/`__builtin_clzll` for
   `_M_ARM64`; `arm/cpu.c` sets NEON unconditionally on `ARCH_AARCH64` and
   probes SVE2 only under `__linux__`, so WoA dispatches NEON; no NEON
   source uses vector-extension indexing, brace-initialised vector types,
   compound-literal vectors or multi-register `_x2`..`_x4` loads.
4. Noted, not fixed here: the x86 carve-outs (`core/src/meson.build`, the
   `x86_*` static libraries) and `libvmaf_psnr_hvs_scalar_static_lib` pass
   `-ffp-contract=off` to `cl.exe` on the x64 MSVC lanes, and
   `core/test/meson.build`'s `_simd_strict_fp_args` does the same for the
   SIMD tests: D9002 noise on every affected TU, numerically inert for the
   reason above. Tracked as `T-MSVC-FFP-CONTRACT-D9002-2026-09-19` in
   `docs/state.md`.

## Required or advisory

ADR-1259 (PR #1485) records the matrix as it runs and makes changing the
required set a decision of its own. A new lane with no green history that
joined the required set would block the rc train on its own teething
problems, and `check-aggregator-names.sh` would need a `# required-aggregator`
marker plus an aggregator entry. The lane starts advisory; the maintainer
decides promotion.

## What the lane does not cover

- CUDA on WoA (needs the coordinated CUDA 13.4 bump).
- clang-cl and llvm-mingw for AArch64 Windows.
- SVE2 at runtime: `arm/cpu.c`'s probe is `__linux__`-gated and the
  hosted runners' CPUs are not advertised as SVE2 anyway.
- The full `meson test` suite; `--suite fast` is the first step and the
  MinGW64 lane keeps running the whole suite on Windows x64.
