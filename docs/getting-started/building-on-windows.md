# Building on Windows

Windows has three build routes. This page covers the first and the details of
the other two; the [Windows MSVC install page](install/windows.md) has the
setup script for MSVC.

| Route | Compiler | GPU backends | CI job ([`libvmaf-build-matrix.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/libvmaf-build-matrix.yml)) |
| --- | --- | --- | --- |
| [MSYS2 / MinGW-w64](#msys2-and-mingw-w64) | GCC (UCRT64) | none | `Windows UCRT64`: builds and runs the tests |
| [Native MSVC, x64](#native-msvc-and-cuda) | `cl.exe` | CUDA; SYCL with `icx-cl` | `Windows MSVC+CUDA`, `Windows MSVC+SYCL`: build and install, no GPU tests |
| [Native MSVC, ARM64](#native-msvc-on-windows-arm64) | `cl.exe` for ARM64 | none | `Windows ARM64 MSVC`: builds and runs the fast tests |

The Python harness is the same on every platform: create a virtual environment
and run `pip install python/` from the repository root.

## MSYS2 and MinGW-w64

1. Install [MSYS2](https://www.msys2.org/).
2. Open an **MSYS2 UCRT64** shell and install the toolchain:

    ```bash
    pacman -S --noconfirm --needed \
      mingw-w64-ucrt-x86_64-gcc \
      mingw-w64-ucrt-x86_64-meson \
      mingw-w64-ucrt-x86_64-ninja \
      mingw-w64-ucrt-x86_64-nasm \
      mingw-w64-ucrt-x86_64-pkg-config
    ```

3. From the repository root in the same shell, configure a static build. This
   example installs into `C:/vmaf-install`; change `--prefix` for another
   location:

    ```bash
    meson setup build core \
      --buildtype release \
      --default-library static \
      --prefix C:/vmaf-install \
      -Denable_cuda=false -Denable_sycl=false
    ```

4. Build, install and test:

    ```bash
    meson install -C build
    python3 scripts/ci/run_meson_test.py -- -C build
    ```

The 32-bit MINGW32 environment is not supported: its scores do not match. The
CI job also runs `scripts/ci/check-win64-stack-alignment.py`, because GCC's
MinGW target can emit AVX-512 spills that the Windows x64 stack alignment does
not allow ([ADR-1254](../adr/1254-win64-cannot-realign-the-stack.md)).

## Native MSVC and CUDA

The native MSVC CUDA build compiles, links and installs in CI; the hosted
Windows runner has no GPU, so it runs no GPU scoring tests.

CUDA needs Visual Studio Build Tools and the Windows SDK, even when the host
library is built with MinGW. NVCC's host compiler (`-ccbin`) is chosen in this
order:

1. when the build compiles C++ with MSVC (a Native Tools Command Prompt or
   `vcvarsall.bat`), that same `cl.exe`, so the host half of every `.cu` file
   uses the same toolset and standard library as the rest of the build;
2. otherwise the newest MSVC toolset of the latest Visual Studio install that
   `vswhere` reports (an install can carry older toolsets beside the current
   one, such as 14.29 in Visual Studio 2026, whose library cannot compile the
   C++20 headers the CUDA kernels use);
3. if that finds nothing, `cl.exe` on `PATH`.

The compiler it picks is also used for MSVC header discovery; configuration
prints which one. If no place has a compiler, configuration stops and says
that Visual Studio Build Tools are required.

A regression test checks this lookup on any POSIX host, without a Windows SDK
or a GPU, using stubbed compiler responses:

```sh
python3 core/test/test_windows_cuda_compiler_discovery.py -v
```

It also runs in the Meson `fast` suite. Native compilation and GPU runtime
validation are separate checks.

## Native MSVC on Windows ARM64

libvmaf builds natively on Windows on ARM64 with the ARM64-hosted MSVC
toolset. The NEON kernels under `core/src/feature/arm64/` are compiled and
used, as on Linux and macOS AArch64. CI builds this recipe on the
`windows-11-vs2026-arm` runner
([ADR-1260](../adr/1260-windows-arm64-cpu-lane.md)).

You need:

- Visual Studio 2022 17.4 or later (CI uses 2026) with the **MSVC ARM64/ARM64EC
  build tools** component;
- Python 3.11 or later, with `pip install meson ninja`;
- no NASM: ARM64 has no assembly sources.

Build from an **ARM64 Native Tools Command Prompt**, or after running
`vcvarsall.bat arm64` from Visual Studio's `VC\Auxiliary\Build` directory:

```bat
cd <vmaf-repo-root>
set CFLAGS=/experimental:c11atomics
set CXXFLAGS=/experimental:c11atomics
meson setup build core --buildtype release ^
  --prefix %CD%\install ^
  --default-library=static ^
  -Dc_std=none ^
  -Denable_cuda=false -Denable_sycl=false ^
  -Denable_float=true
ninja -C build install
python scripts\ci\run_meson_test.py -- -C build --suite fast
```

`/experimental:c11atomics` is required on every MSVC build: libvmaf uses C11
atomics, and MSVC's `<stdatomic.h>` refuses them without it.

To confirm the toolset:

- `cl.exe` prints `for ARM64` in its banner when the ARM64-hosted toolset is
  active.
- The x64 cross toolset (`vcvarsall.bat amd64_arm64`) also produces ARM64
  binaries, but runs the compiler under emulation.
- `install\bin\vmaf.exe` is an ARM64 image (PE machine `0xAA64`).

How this build differs from the other AArch64 builds:

- **Strict floating point.** GCC and clang get `-ffp-contract=off` for the
  float NEON carve-outs; MSVC gets `/fp:precise`, which generates no fused
  multiply-adds by default (`arm64_strict_fp_args` in `core/src/meson.build`).
- **No SVE2.** MSVC has no `<arm_sve.h>`, and SVE2 is probed at run time on
  Linux only, so the binary dispatches NEON.
- **No CUDA.** NVIDIA published no Windows ARM64 packages for CUDA 13.3.1, the
  release this was checked against.

## Threads on MSVC

MSVC ships no `pthread.h`, so an MSVC, clang-cl or icx-cl build takes its
threads from a small header in the repository,
`core/src/compat/win32/pthread.h`, which maps the POSIX calls libvmaf makes
onto Windows primitives (slim reader/writer locks, condition variables,
one-time initialisation and `_beginthreadex`). Nothing has to be installed or
configured: Meson selects it whenever the compiler has no `pthread.h`. MinGW-w64
builds use MinGW's own winpthreads instead. Timed waits are supported:
`pthread_cond_timedwait()` takes the usual absolute `CLOCK_REALTIME` deadline
and returns `ETIMEDOUT` once it has passed, and the VMAFx API's host fences
(`vmafx_fence_wait()`) wait on it rather than polling. Upstream's bundled
pthread-win32 and its `-Dbundled_winpthreads` option are not used.

## Library files of an MSVC build

A static MSVC, clang-cl or icx-cl build (`--default-library=static`, as every
recipe on this page uses) installs its two libraries under the names the MSVC
linker gives `-lvmaf` and `-lvmafx`:

| File | What it holds |
| --- | --- |
| `lib\vmaf.lib` | the libvmaf API (`vmaf_*`), on top of libvmafx |
| `lib\vmafx.lib` | the engine and the VMAFx API (`vmafx_*`) |
| `lib\pkgconfig\libvmaf.pc`, `libvmafx.pc` | `Libs: -lvmaf` with `Requires: libvmafx`, and `Libs: -lvmafx` |

A consumer links both: `vmaf.lib vmafx.lib`, or what
`pkg-config --libs --static libvmaf` prints. FFmpeg's MSVC toolchain
(`--toolchain=msvc`) turns `-lvmaf` into `vmaf.lib` and finds the files without
renaming. MinGW builds keep GCC's names (`libvmaf.a`, `libvmafx.a`), and so does
an MSVC build with `--default-library=both`, where `vmaf.lib` is the import
library of `vmaf.dll`.

## FFmpeg with MSVC

FFmpeg builds with `cl.exe` through its own configure script
(`--toolchain=msvc`), run from an MSYS2 shell that has the MSVC environment.
The patch series in `ffmpeg-patches/` applies and links on this route; the
`FFmpeg Windows MSVC` check
([`ffmpeg-integration.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/ffmpeg-integration.yml))
builds it whenever a change reaches the C library or the patches
([ADR-2783](../adr/2783-ffmpeg-msvc-consumer-leg.md)).

1. From an **x64 Native Tools Command Prompt** in the repository root, build
   and install VMAFx as a static library:

    ```bat
    set CFLAGS=/experimental:c11atomics
    set CXXFLAGS=/experimental:c11atomics
    meson setup core core\build --buildtype release ^
      --prefix %CD%\install --default-library=static -Dc_std=none ^
      -Denable_cuda=false -Denable_sycl=false
    ninja -C core\build install
    ```

2. Install [MSYS2](https://www.msys2.org/) and, in a UCRT64 shell, the tools
   FFmpeg's build needs:

    ```bash
    pacman -S --noconfirm --needed make diffutils git \
      mingw-w64-ucrt-x86_64-nasm mingw-w64-ucrt-x86_64-pkgconf \
      mingw-w64-ucrt-x86_64-python
    ```

3. Open the UCRT64 shell from the same Native Tools Command Prompt with
   `msys2_shell.cmd -ucrt64 -use-full-path`, so `cl.exe`, `INCLUDE` and `LIB`
   stay set. MSYS2's `link` command comes first on `PATH`; FFmpeg's
   `compat/windows/mslink` runs MSVC's `link.exe` from beside `cl.exe`
   instead, so nothing needs renaming.
4. From the repository root, build the configured FFmpeg release with the
   whole series against the install, and compare the filter's scores with the
   CLI's:

    ```bash
    FFMPEG_TOOLCHAIN=msvc VMAF_SCORE_CHECK=1 \
      VMAF_PREFIX="$PWD/install" \
      bash ffmpeg-patches/test/build-and-run.sh
    ```

The script configures FFmpeg with `--extra-cflags=-MD`: every object of one
MSVC link must use the same C runtime, and a release build of VMAFx compiles
with `/MD` (Meson's `b_vscrt=from_buildtype`). A VMAFx build with another
runtime (`-Db_vscrt=mt`, a debug build's `/MDd`) needs FFmpeg built with the
same one; the linker reports a mismatch as warning `LNK4098`, which the
script's warning check fails on.

With `FFMPEG_TOOLCHAIN=msvc` that check covers the lines the patch series adds
or changes and every linker warning. `cl.exe` reports some 400 warnings in
FFmpeg's own sources at the configured release (`C4334`, `C4113`, `C5287` and
others), some of them in files the series also edits, and `D9024` when FFmpeg
links its host tools; the series does not write those lines, so they are
printed but do not fail the run. With the host's compiler
every warning fails it. The script's other settings (`FFMPEG_JOBS`,
`SMOKE_FATE`, `KEEP_BUILD`) are listed at the top of
[`ffmpeg-patches/test/build-and-run.sh`](https://github.com/VMAFx/vmafx/blob/master/ffmpeg-patches/test/build-and-run.sh).
