# ADR-0121: Windows GPU build-only matrix legs (MSVC + CUDA, MSVC + oneAPI SYCL)

- **Status**: Accepted
- **Date**: 2026-04-19
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, build, cuda, sycl, github-actions

## Context

The fork's GPU backends (CUDA + SYCL) are exercised in CI **only on
Linux**. The existing GPU matrix entries in
[libvmaf-build-matrix.yml](../../.github/workflows/libvmaf-build-matrix.yml)
all run on `ubuntu-latest` with gcc as the C/C++ host compiler:

- `Build — Ubuntu CUDA` (gcc + nvcc)
- `Build — Ubuntu SYCL` (gcc host + icpx for SYCL .cpp)
- `Build — Ubuntu SYCL + CUDA`
- `Build — Ubuntu CUDA Static`, `Build — Ubuntu SYCL Static`

The single existing Windows job is `Build — Windows MinGW64 (CPU)` —
MSYS2 / MinGW-w64 with `-Denable_cuda=false -Denable_sycl=false`. So
**no** CI leg currently verifies that the CUDA host code or the SYCL
.cpp sources even *compile* against the MSVC ABI.

This is a real coverage hole:

1. Windows is a tier-1 platform for the libvmaf binary distribution
   (vmaf.exe artifact is uploaded by the MinGW job and consumed by
   downstream users).
2. The CUDA backend's host code (`core/src/cuda/*.c`) uses POSIX
   patterns (e.g. `pthread_*` aliases, file descriptors for dma-buf
   import paths) that need conditional `#ifdef _WIN32` guards. A
   Linux-only CI cannot catch a regression that breaks the MSVC build.
3. The SYCL backend's `vmaf_sycl_*` C-API entry points
   (`core/src/sycl/`) similarly need to compile cleanly under
   `icx-cl` (the Windows DPC++ driver), which has subtly different
   `__declspec(dllexport)` and CRT-linkage requirements than the
   Linux `icpx` driver.
4. Downstream Windows users who try `meson setup -Denable_cuda=true`
   on Windows currently hit unguarded build breakage that we discover
   only via downstream issue reports.

The user explicitly scoped this PR as **"Add both CUDA + SYCL Windows
build-only legs"** (see References) — the most ambitious of the
three options I offered. "Build-only" because `windows-latest` GitHub
runners have no GPU hardware, so executing GPU kernels is not
possible; what we *can* do is verify compile + link.

## Decision

Add a new top-level `windows-gpu-build` job in
[libvmaf-build-matrix.yml](../../.github/workflows/libvmaf-build-matrix.yml)
running on `windows-latest`, with two matrix entries:

| Display name | Backend | Toolchain | Build-only? |
| --- | --- | --- | --- |
| `Build — Windows MSVC + CUDA (build only)` | CUDA | MSVC + nvcc 13.0.0 | yes |
| `Build — Windows MSVC + oneAPI SYCL (build only)` | SYCL | MSVC + icx-cl 2025.3 | yes |

Both legs:

- Use [`ilammy/msvc-dev-cmd@v1`](https://github.com/ilammy/msvc-dev-cmd)
  to set up the MSVC dev environment (community-standard action that
  runs `vcvars64.bat` and exports the env vars). Required because
  nvcc shells out to `cl.exe` and `icx-cl` is a `cl.exe`-compatible
  driver.
- Pin CUDA tooling to the exact same version as the Linux CUDA leg:
  CUDA 13.0.0 (via [`Jimver/cuda-toolkit@v0.2.35`](https://github.com/Jimver/cuda-toolkit)).
  Identical pins keep the MSVC-vs-Linux comparison meaningful — if
  a build breaks here but not on Linux, it's an MSVC ABI issue, not
  a tooling-version delta.
- Install oneAPI on Windows via the official **`oneapi-src/oneapi-ci`
  offline-installer pattern**: `curl` the Intel Base Toolkit
  Windows installer, extract the bootstrapper, run with
  `--components=intel.oneapi.win.cpp-dpcpp-common --eula=accept`
  plus `NEED_VS{2017,2019,2022}_INTEGRATION=0` to skip the (slow)
  Visual Studio plug-in steps. Pinned to BaseKit 2025.3.0.372 to
  match the Linux SYCL leg's `intel-oneapi-compiler-dpcpp-cpp-2025.3`.
  Two earlier candidates failed: (1)
  [`rscohn2/setup-oneapi@v0`](https://github.com/rscohn2/setup-oneapi)
  is **Linux-only** — every installer URL in its `src/main.js`
  ends in `.sh`; (2) Chocolatey's `intel-oneapi-basekit` package
  is **not on the community feed** (verified 2026-04-19: lookup
  fast-failed in 3 s with `package was not found with the
  source(s) listed`). The Intel offline installer is what Intel
  themselves use in CI
  (`oneapi-src/oneapi-ci/scripts/install_windows.bat`), so it
  gives us the most stable, owner-blessed install path on Windows.
- Inject `/experimental:c11atomics` into `CFLAGS` and `CXXFLAGS`
  before `meson setup` on Windows. libvmaf uses C11 atomics
  (`stdatomic.h` + `__atomic_*`); MSVC's `<stdatomic.h>` errors
  with `"C atomic support is not enabled"` unless the
  `/experimental:c11atomics` compiler flag is set — the flag is
  opt-in until MSVC ships full C11/C17 atomics support. Setting
  it via env var is preferable to a meson native file because
  the flag is purely build-system-conditional, not source-side:
  gcc / clang / icpx / nvcc don't need it.
- Pull two extra dependencies that the Linux GPU legs get
  for free from apt but Windows installers do not ship:
  - **CUDA `crt` sub-package** — added to the
    `Jimver/cuda-toolkit` `sub-packages` list. Provides
    `crt/host_config.h` (CUDA Runtime Library compile-time
    headers) which `cuda_runtime.h` includes unconditionally.
    Without it, the very first MSVC translation unit that
    touches the CUDA runtime errors with `Cannot open include
    file: 'crt/host_config.h'`. (`cuda_cccl` looks like the
    intuitive name from the docs but is **not** a valid Windows
    sub-package — the installer rejects it with exit code
    `0xE0E07F19`. The Windows installer name is the bare
    `crt`.)
  - **CUDA `nvvm` sub-package** — also added to the
    `sub-packages` list. Ships `nvvm/bin/cicc.exe` (CUDA's
    LLVM-based device compiler) and `nvvm/libdevice/libdevice.*.bc`
    (the device library nvcc links against). Without it, the
    `nvcc` driver binary installs fine but crashes at the first
    `.cu → PTX` stage with `stderr: The system cannot find
    the path specified.` — because `nvcc.profile` points
    `CICC_PATH = $(TOP)/nvvm/bin` and that directory doesn't
    exist. On Linux the apt `cuda-nvcc-XY` package pulls NVVM
    in transitively; on Windows the Jimver action requires
    naming it explicitly. Discovered by reproducing the CI
    failure locally in a Windows Server 2022 VM (CUDA
    13.0.48, 2026-04-19) after eight rounds of CI-loop
    debugging failed to surface the root cause from log output
    alone.
  - **Level Zero loader from source** — `oneapi-src/level-zero`
    cloned at tag `v1.18.5` (matches the Ubuntu 24.04 apt
    `level-zero-dev` version, keeping the parity invariant with
    the Linux SYCL leg) and built via `cmake --build … --target
    install` to a job-local prefix. Its `include/` and `lib/`
    are appended to `INCLUDE` and `LIB` via `GITHUB_ENV` so
    meson's `cc.find_library('ze_loader', required: true)` at
    `core/src/meson.build:492` resolves. Windows oneAPI
    BaseKit ships the SYCL runtime but not the L0 loader
    `ze_loader.lib`; building from source is the
    Intel-documented path on Windows.
- Make `svml` / `irc` runtime-library lookup Linux-only in
  `core/src/meson.build`. Those explicit `cc.find_library`
  calls exist so a non-Intel host linker (gcc/g++) can resolve
  Intel runtime symbols emitted by icpx-compiled objects. On
  Windows the host C/C++ compiler is **icx-cl** itself — the
  same Intel toolchain that emits those symbols and auto-injects
  svml/irc at link time — and the Windows lib names differ
  (`svml_dispmd.lib`, `libirc.lib`) so the bare-name lookup
  would fail anyway. Guard the block with `if
  host_machine.system() != 'windows'`.
- **Win32 `pthread.h` compat shim** — libvmaf core uses the
  pthread API (mutex / cond / thread-create) across ~14 files,
  with `#include <pthread.h>` unconditional. MSVC and clang-cl
  ship no `pthread.h` (MinGW does, via winpthreads). Round-10
  surfaced this as `fatal error C1083: Cannot open include
  file: 'pthread.h'` in six CUDA host TUs once the nvvm device
  compiler started running. Resolved by adding a header-only
  shim at [`core/src/compat/win32/pthread.h`](../../core/src/compat/win32/pthread.h)
  that maps the in-use pthread subset onto Win32 SRWLOCK +
  CONDITION_VARIABLE + `_beginthreadex`, mirroring the
  long-standing `compat/gcc/stdatomic.h` pattern. Wired in
  via a new `pthread_dependency` declared in
  [`core/meson.build`](../../core/meson.build) and gated on
  `cc.check_header('pthread.h')` failing — so MinGW and POSIX
  paths are untouched. Shim covers `pthread_t`,
  `pthread_mutex_*`, `pthread_cond_*`, `pthread_create`,
  `pthread_join`, `pthread_detach`, plus
  `PTHREAD_MUTEX_INITIALIZER` / `PTHREAD_COND_INITIALIZER` —
  exactly the surface in use, no more. Floor is Vista+ for
  SRWLOCK / CONDITION_VARIABLE; `windows-2022` runners are
  well above that. Build-only legs do not exercise the
  shim's runtime semantics, but the GPU host TUs that link
  against libvmaf now compile cleanly under MSVC.
- **icpx-cl Windows host-arg handling** — SYCL's `icpx` driver
  on Windows targets `x86_64-pc-windows-msvc` and rejects
  `-fPIC` outright (`unsupported option '-fPIC' for target
  'x86_64-pc-windows-msvc'`). PIC is the default for Windows
  DLLs anyway, so dropping the flag on Windows is the correct
  build-system fix, not a workaround. Resolved in
  [`core/src/meson.build`](../../core/src/meson.build) by
  introducing `sycl_pic_arg = host_machine.system() != 'windows'
  ? ['-fPIC'] : []` and threading it into both
  `sycl_common_args` and `sycl_feature_args` in place of the
  hard-coded `-fPIC` token. The same SYCL `.cpp` translation
  units transitively include `feature_collector.h`, which pulls
  in `pthread.h` — so the Win32 pthread shim path is also
  appended to `sycl_inc_flags` on Windows, mirroring the
  `cuda_extra_includes` handling for the nvcc fatbin
  `custom_target`. icpx `custom_target` invocations bypass
  meson's regular `dependencies:` plumbing the same way nvcc
  fatbins do, so the include-path threading must be explicit.
- Skip the test step entirely. `windows-latest` has no GPU; running
  even CPU-only tests would consume runner minutes for no signal
  beyond what the Linux legs already provide.

The two job names are pinned to **required** status checks on
`master` immediately after this PR's merge (21 → 23 contexts;
counting the two Linux DNN legs from ADR-0120 if that PR landed
first).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Status quo (Linux GPU only)** | Zero CI cost. | MSVC build-portability regressions only surface from downstream user reports. | The whole motivation for this ADR is closing exactly that hole. |
| **Add only the CUDA Windows leg** | Half the CI minutes; CUDA is the more popular GPU backend. | SYCL on Windows is the less-tested backend → most likely to bit-rot → most valuable to gate. | Half coverage of the hole isn't satisfying; user scope was both. |
| **Add Windows GPU legs but as `experimental: true` (informational)** | Avoids gating master on Windows GPU runner flakiness (Jimver/cuda-toolkit network occasionally times out). | Defeats the purpose: an informational green-vs-yellow distinction won't surface PR regressions to authors who don't notice yellow. | Pin them as required; flakiness is rare enough to absorb the occasional re-run. |
| **Run actual tests via Windows GPU self-hosted runner** | Real GPU-on-CI coverage, not just compile/link. | Requires self-hosted runner infrastructure with a discrete GPU; security/maintenance overhead disproportionate to the benefit; out of scope for this PR. | Out of scope. Could be a separate ADR if the fork ever justifies a self-hosted Windows GPU host. |
| **Use cmake instead of meson on Windows** | cmake has slightly better MSVC integration historically. | The fork has already standardised on meson everywhere; introducing cmake just for Windows GPU legs would create an unmaintained second build path. | Meson works fine on Windows + MSVC; just need correct env vars. |

## Consequences

**Positive:**

- MSVC + CUDA build regressions and MSVC + oneAPI SYCL build
  regressions surface in CI on the PR that introduces them, not from
  downstream user reports months later.
- Windows tier-1 status is upheld for the GPU backends, not just for
  CPU.
- The `vmaf.exe` artifact uploaded by the MinGW CPU job and the
  (eventual, future) MSVC GPU `vmaf.exe` artifact share the same
  test-portability story going forward.

**Negative:**

- Two additional `windows-latest` matrix runs per PR. Each Windows
  runner is ~2× the cost of a Linux runner in GHA minutes. Estimated
  cost: ~25 min wall-clock added per CI run (parallel across the
  matrix), ~50 GHA minutes added per run. Acceptable on the public
  fork's free tier.
- Build-only ≠ runtime-tested. A regression that compiles cleanly
  but produces wrong output on Windows GPUs would still slip through.
  Mitigated by Linux GPU legs catching most behavioural regressions
  via the Ubuntu CUDA / SYCL legs.
- One new third-party action in the workflow:
  `ilammy/msvc-dev-cmd@v1`. Widely used with a good security
  record, but additional supply-chain surface beyond the existing
  `Jimver/cuda-toolkit`. The SYCL leg additionally pulls a
  signed installer from
  `registrationcenter-download.intel.com` over HTTPS — Intel-owned
  infrastructure, the same source `oneapi-src/oneapi-ci` uses.
- The Intel offline-installer URL hard-codes the BaseKit version
  (`2025.3.0.372`) and a per-release directory id. When the Linux
  SYCL leg bumps oneAPI, this URL must be updated in lockstep —
  drift defeats the parity invariant. See `docs/rebase-notes.md`
  entry 0022 for the touch-list.

**Neutral / follow-ups:**

- Branch protection re-pinned atomically with this ADR's merge to add
  `Build — Windows MSVC + CUDA (build only)` and
  `Build — Windows MSVC + oneAPI SYCL (build only)` as required
  contexts.
- A future ADR may add a Windows GPU runtime-test job once a
  self-hosted Windows GPU runner is justified.
- A future ADR may pin the third-party actions to commit SHAs
  (consistent with whatever SHA-pinning policy the rest of the repo
  adopts).

## References

- [ADR-0115](0115-ci-trigger-master-only-and-matrix-consolidation.md) —
  matrix consolidation; this ADR adds a new job to the consolidated
  workflow.
- [ADR-0116](0116-ci-workflow-naming-convention.md) — Title Case
  display names; the two new legs follow that convention.
- [ADR-0120](0120-ai-enabled-ci-matrix-legs.md) — sister ADR (DNN-on
  matrix legs) landed immediately before this one.
- [ADR-0037](0037-master-branch-protection.md) — branch-protection
  policy that the post-merge re-pin updates.
- [ilammy/msvc-dev-cmd@v1](https://github.com/ilammy/msvc-dev-cmd)
- [Jimver/cuda-toolkit@v0.2.35](https://github.com/Jimver/cuda-toolkit)
- [Intel `oneapi-src/oneapi-ci` — official Windows install pattern](https://github.com/oneapi-src/oneapi-ci/blob/master/scripts/install_windows.bat)
- [NVIDIA Windows CUDA sub-package list](https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/index.html#install-cuda-software)
- [MSVC `/experimental:c11atomics` opt-in flag](https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version)
- `req` (paraphrased): user picked "Add both CUDA + SYCL Windows
  build-only legs" via the post-cascade scope popup, after I offered
  it as the most-ambitious of three scope choices for Windows GPU
  coverage.
- Per-surface doc impact: this ADR documents the workflow-file change
  and the branch-protection delta. The CUDA / SYCL backends
  themselves are unchanged; no `docs/backends/` edit needed beyond
  the ADR.
