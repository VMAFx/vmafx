<!-- markdownlint-disable MD060 -->
# ADR-1260: Windows on ARM64 CPU build-and-test lane

- **Status**: Proposed
- **Date**: 2026-09-19
- **Deciders**: Lusoris
- **Tags**: ci, build, arm64, windows, simd, fork-local

## Context

No CI lane has ever compiled the fork's AArch64 code with MSVC. The `Ubuntu
ARM clang` lane and the macOS lanes build `core/src/feature/arm64/` with clang,
and the three Windows lanes (`Windows MinGW64`, `Windows MSVC+CUDA`,
`Windows MSVC+SYCL`) build the x86 tree only. So everything under
`core/src/feature/arm64/`, every `#if ARCH_AARCH64` branch in the shared
extractors, `core/src/arm/cpu.c` and the AArch64-gated tests under
`core/test/` have never met `cl.exe`. ADR-1040 speaks of a "Windows arm64"
build being clean; there was no such lane, and no workflow uses a Windows ARM
runner.

The maintainer flagged Windows on ARM as a to-do before `1.0.0-rc.1` after
CUDA 13.4 added `windows-arm64` packages: NVIDIA's `redistrib_13.4.2.json`
lists `windows-arm64` for `cuda_nvcc` and `cuda_cudart` in 13.4.1
(2026-09-09) and 13.4.2 (2026-09-16), while 13.3.1, the pin every CUDA lane
uses, has none. The CUDA bump is a separate, blocked item
(`Jimver/cuda-toolkit` PR #448 is open), so a CUDA-on-WoA leg is out of scope
here; the CPU lane is what can land now.

What GitHub offers, checked on 2026-09-19 against `actions/runner-images`
`README.md` and the two image READMEs, and against the public-repository
runner table on docs.github.com:

- `windows-11-arm`: Windows 11 Arm64 with Visual Studio Enterprise 2022
  17.14. Runner-images issue #14602 migrates this label to the Visual Studio
  2026 image "over a week beginning September 21, 2026", completing by
  2026-09-30.
- `windows-11-vs2026-arm`: Windows 11 Arm64 with Visual Studio Enterprise
  2026 18.9, generally available since 2026-08-19 (issue #14592).
- Both are free and unlimited for public repositories (4 vCPU, 16 GB). Both
  carry `Microsoft.VisualStudio.Component.VC.Tools.ARM64`, Python 3.13.15,
  Ninja 1.13.2, CMake 4.4.3 and LLVM 22.1.8; neither ships meson or nasm.
- `windows-2025`, the label of the x64 Windows lanes, now resolves to the
  `windows-2025-vs2026` image, so every MSVC lane is on the same major.

What the tree needed for that lane (found by reading, then confirmed by the
lane):

- `core/test/test_ciede_neon.c` implemented its guard-page over-read probe
  with `<sys/mman.h>`, `<unistd.h>`, `sigaction` and `sigsetjmp` inside
  `#if ARCH_AARCH64`. That is POSIX only and this is the first AArch64
  build with a non-POSIX libc.
- `core/src/meson.build` handed `-ffp-contract=off` to whatever compiler
  built the float NEON carve-outs and `-march=armv9-a+sve2` to the SVE2
  probe. `cl.exe` reports `D9002: ignoring unknown option` for both and
  goes on; the SVE2 probe could not succeed on MSVC (no `<arm_sve.h>`).
- Inert on MSVC and left alone: the `#pragma GCC diagnostic` blocks and the
  `__attribute__((optimize("-ffp-contract=off")))` in the NEON sources are
  guarded on `__GNUC__ && !__clang__`; `compat_builtin.h` already provides
  `__builtin_clz*` under `_M_ARM64`; `arm/cpu.c` probes SVE2 only under
  `__linux__`, so Windows reports NEON alone; the test arch lists contain
  `aarch64`, which is what meson reports for Windows ARM64
  (`mesonbuild/envconfig.py` maps `arm64` to `aarch64`).

## Decision

We add a `windows-arm64` job, display name `Windows ARM64 MSVC`, to
`.github/workflows/libvmaf-build-matrix.yml`:

- Runner `windows-11-vs2026-arm`. The explicit label pins one image while
  `windows-11-arm` is mid-migration; after 2026-09-30 the two are the same
  image and the label may follow the x64 legs' convention of naming the OS
  rather than the Visual Studio version.
- Toolchain: `TheMrMilchmann/setup-msvc-dev` with `arch: arm64`. The action
  forwards `arch` unchanged to `vcvarsall.bat` (`src/setup-msvc-dev.ts`),
  and `arm64` selects the ARM64-hosted native toolset
  (`VC\Auxiliary\Build\vcvarsarm64.bat`, present since Visual Studio 2022
  17.4). `amd64_arm64` would run the x64-hosted cross compiler under
  emulation.
- Python 3.14.7 from `actions/setup-python`, the same pin as the x64 legs
  (`actions/python-versions` publishes `win32`/`arm64` builds of it);
  `pip install meson ninja` (`ninja` 1.13.2 has a `win_arm64` wheel). No
  nasm: `core/src/meson.build` probes NASM only under
  `cpu_family().startswith('x86')`.
- Configure as the x64 MSVC legs do: `/experimental:c11atomics`,
  `--default-library=static`, `-Denable_float=true`, CUDA and SYCL off;
  `enable_asm` at its default, which on AArch64 means the NEON TUs.
- A PE-header check that `install\bin\vmaf.exe` carries machine `0xAA64`
  (ARM64). An x64 binary would run under emulation and pass the tests, so
  this is what proves the lane exercised native code.
- `meson test --suite fast --print-errorlogs`, and the `vmaf.exe` artifact.

The lane is **advisory**: it is not in `required-aggregator.yml` and carries
no `# required-aggregator` marker. ADR-1259 (PR #1485) makes changing the
required set a decision of its own; promotion is the maintainer's call once
the lane has a green history, and is recorded by amending this ADR's status
and the aggregator together.

This ADR amends the `libvmaf-build-matrix.yml` table of ADR-1259 with one
row:

| Lane | What it runs | Status | Owning record | Notes |
| --- | --- | --- | --- | --- |
| `Windows ARM64 MSVC` | native ARM64 MSVC CPU build on `windows-11-vs2026-arm`, PE machine check, meson `fast` suite | not required | ADR-1260 | CPU only until the CUDA 13.4 bump |

With the lane come the two tree changes it needed:

- `core/test/test_ciede_neon.c` keeps its guard-page probe on every platform
  through five entry points (`probe_page_size`, `guarded_row_alloc`,
  `guarded_row_free`, `fault_trap_install`/`fault_trap_restore`,
  `run_kernel_guarded`): `mmap` + `PROT_NONE` + `sigsetjmp` on POSIX,
  `VirtualAlloc` + `PAGE_NOACCESS` + SEH `__try`/`__except` on Windows. The
  rewrite also removes the file's `goto`s and keeps every function under 60
  lines (HISS-01, HISS-04). Verified byte-for-byte equivalent on the Linux
  AArch64 side under `qemu-aarch64-static` (4 of 4 checks pass before and
  after).
- `core/src/meson.build` introduces `arm64_strict_fp_args`: `/fp:precise` on
  `msvc`, `-ffp-contract=off` on every other compiler, used by the four NEON
  and two SVE2 float carve-outs. Microsoft documents that under
  `/fp:precise`, the default, "floating-point contractions aren't generated
  by default" since Visual Studio 2022, so the flag carries the carve-outs'
  intent on MSVC instead of a warning. The SVE2 probe is skipped on `msvc`.

- `core/src/feature/simd_dx.h` gates both NEON macro blocks on
  `__ARM_NEON` or `_M_ARM64` / `_M_ARM64EC`, and its NEON spill buffer uses
  the `alignas` keyword. MSVC defines no `__ARM_NEON`, so the header was
  empty on this lane: `ssim_neon.c` failed to compile and
  `convolve_neon.c` silently degraded its ADR-0138 widening reduction to an
  implicit external call (C4013).

That last defect is why this lane runs tests instead of only building, as the
ADR-0121 Windows legs do. `ssim_neon.c` failing to compile would have stopped a
build-only lane too, but the `convolve_neon.c` degradation would not: C4013 is a
warning, the object links, and a build-only lane reports the step green while a
bit-exact reduction has quietly become a call to a function that does not exist.
The lane's first run found it. A lane that only builds would not have.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `windows-11-vs2026-arm`, MSVC ARM64 native, advisory, `fast` suite (chosen) | one image during the migration window; same compiler family as the x64 Windows legs; NEON executed under MSVC codegen | one more lane per PR; `fast` is a subset | — |
| `windows-11-arm` label | names the OS like the x64 legs' `windows-2025` | between 2026-09-21 and 09-30 a run may land on VS 2022 or VS 2026 | revisit after the migration completes |
| clang-cl / LLVM 22 from the image | closer to the compilers that already build the AArch64 tree; accepts `-ffp-contract=off` | does not test the MSVC toolchain the fork ships Windows builds with; clang-cl's `/fp:precise` keeps in-statement contraction | MSVC is the point of the lane |
| `amd64_arm64` cross toolset via `vcvarsall` | listed in the msvc-170 documentation table | x64-hosted compiler under emulation; proves nothing about the ARM64-hosted tools | the native toolset exists since 17.4 |
| Build-only, as the ADR-0121 legs | shorter | the ADR-0121 legs skip tests because the runner has no GPU; here the CPU is the device and executing NEON under MSVC is the value | tests stay |
| Full `meson test` from the start | maximal coverage | untested MSVC-specific failures across ~4,000 test registrations with a five-push budget; the MinGW64 lane already runs the full suite on Windows x64 | `fast` first, full suite as a follow-up |
| Required from day one | regressions block immediately | no green history; a new lane that flakes blocks the rc train | advisory first, promotion by the maintainer |
| Skip the read-bounds probe on Windows in `test_ciede_neon.c` | less code | the lane would lose the over-read coverage that motivated the test | the Windows path is forty lines |

## Consequences

- **Positive**: MSVC ARM64 codegen and NEON execution are covered on every
  non-draft PR and master push; a native Windows ARM64 `vmaf.exe` is built
  per run; `docs/getting-started/building-on-windows.md` gains an ARM64
  recipe that CI exercises.
- **Negative**: one more Windows lane per PR (a cold MSVC build plus the
  `fast` suite); the lane is advisory, so a red run needs a human to look.
- **Neutral / follow-ups**: promotion to required; a CUDA-on-WoA leg once
  the CUDA 13.4 bump lands (`Jimver/cuda-toolkit` PR #448); running the full
  suite; switching to `windows-11-arm` after 2026-09-30; the x86 carve-outs
  and `libvmaf_psnr_hvs_scalar` still pass `-ffp-contract=off` to `cl.exe`
  on the x64 lanes (D9002 noise, numerically inert), tracked in
  `docs/state.md`.

## References

- paraphrased: the maintainer noted on 2026-09-19 that CUDA 13.4 adds
  Windows on ARM and called Windows ARM support a to-do before rc1.
- Research digest: [Research-2066](../research/2066-windows-arm64-lane.md).
- `actions/runner-images`: `README.md` label table; issues #14592 and
  #14602; `images/windows/Windows11-Arm64-Readme.md` and
  `images/windows/Windows11-VS2026-Arm64-Readme.md` (image versions
  20260906.161.1 and 20260907.151.1).
- docs.github.com, "GitHub-hosted runners reference", table "Standard
  GitHub-hosted runners for public repositories".
- `TheMrMilchmann/setup-msvc-dev` `src/setup-msvc-dev.ts` (`vcvarsall.bat
  ${arch}`, `normalizeArch` passes `arm64` through).
- Microsoft Learn, "/fp (Specify floating-point behavior)", `/fp:precise`
  and `/fp:contract`; "Use the Microsoft C++ toolset from the command line",
  `vcvarsall` syntax and `vcvarsarm64.bat`.
- `actions/python-versions` `versions-manifest.json` (3.14.7 `win32`
  `arm64`); PyPI `ninja` 1.13.2 (`win_arm64` wheel).
- NVIDIA `redistrib_13.4.2.json` (`windows-arm64` for `cuda_nvcc`,
  `cuda_cudart`); `Jimver/cuda-toolkit` PR #448.
- Related: ADR-1259 and ADR-1258 (both on PR #1485, not yet on master when
  this ADR was written, hence no link; ARM64 is 64-bit, so ADR-1258's
  64-bit-only rule and this lane do not conflict),
  [ADR-0121](0121-windows-gpu-build-only-legs.md),
  [ADR-0664](0664-windows-cuda-toolkit-installer.md),
  [ADR-1040](1040-integer-ssim-moments-type-non-x86.md),
  [ADR-0873](0873-arm64-neon-bit-exactness-audit.md),
  [ADR-1057](1057-revert-float-adm-simd-dispatch-neon-fma.md),
  [ADR-1142](1142-whole-codebase-standards.md),
  [ADR-1234](1234-local-preflight-gate.md).
