<!-- markdownlint-disable MD013 -->
# SYCL on Windows (native MSVC + oneAPI build)

This page builds `vmaf.exe` with the SYCL backend natively on Windows and runs
it on an Intel GPU. It follows the `Windows MSVC+SYCL` CI leg in
[`.github/workflows/libvmaf-build-matrix.yml`](../../../.github/workflows/libvmaf-build-matrix.yml).
That leg has no GPU, so it builds, checks that the SYCL kernels are
registered, and stops there; this page is the path that runs them. It was
checked on an i9-12900K with an Arc B580 and a UHD 770
([Research-2125](../../research/2125-windows-native-sycl-run.md)).

Host builds are for development and diagnosis. Published binaries come from
the container ([ADR-1102](../../adr/1102-phase4b9-container-only-publishing.md)).

## What you need

| Tool | Where it comes from | Why |
| --- | --- | --- |
| Visual Studio 2022 or 2026, "Desktop development with C++" workload | Visual Studio installer | MSVC linker, headers and libraries, Windows SDK |
| Intel oneAPI Base Toolkit, DPC++/C++ compiler | Intel offline installer (CI pins 2025.3.0; 2025.1.1 also works) | `icx-cl`, `icpx`, the SYCL runtime and `ocloc`, which the Windows compiler ships |
| Intel GPU driver | Intel Arc / Graphics driver | Level Zero runtime (`ze_loader.dll` in `System32`) |
| Python 3.14, Meson and Ninja | `python -m pip install --require-hashes -r requirements/locks/build.txt` | build system |
| CMake | `python -m pip install cmake` | builds the Level Zero loader below |
| NASM 2.14 or newer | `scoop install nasm` (or winget) | x86 SIMD kernels |
| `xxd` | Git for Windows (`C:\Program Files\Git\usr\bin`) | embeds the built-in models; without it `--model version=...` has nothing to load |

The oneAPI installer for Windows ships no Level Zero import library, so build
the loader once, pinned to the version the CI leg uses. Use the Ninja
generator: the loader's CMake project turns on `/Qspectre`, and the Visual
Studio generator then refuses to build unless the Spectre-mitigated libraries
are installed (`MSB8040`).

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
git clone --depth=1 --branch v1.34.0 https://github.com/oneapi-src/level-zero level-zero-src
cmake -G Ninja -S level-zero-src -B level-zero-src\build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%CD%\level-zero-prefix
cmake --build level-zero-src\build --target install
```

## Build

Run everything in one `cmd` window. The order matters: MSVC first, then oneAPI.

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
call "C:\Program Files (x86)\Intel\oneAPI\setvars.bat"
set INCLUDE=%CD%\level-zero-prefix\include;%INCLUDE%
set LIB=%CD%\level-zero-prefix\lib;%LIB%
set "PATH=%PATH%;C:\Program Files\Git\usr\bin"
set CFLAGS=/experimental:c11atomics
set CXXFLAGS=/experimental:c11atomics
set CC=icx-cl
set CXX=icx-cl

meson setup build core --buildtype release --default-library=static ^
  -Denable_float=true -Dcpp_std=c++latest -Denable_cuda=false -Denable_sycl=true
ninja -C build
```

Why each setting is there:

- `--default-library=static`: libvmaf has no `__declspec(dllexport)`, so an
  MSVC DLL build exports nothing ([ADR-0121](../../adr/0121-windows-gpu-build-only-legs.md)).
- `/experimental:c11atomics`: MSVC's `<stdatomic.h>` refuses C11 atomics
  without it.
- `-Dcpp_std=c++latest`: the C++ sources need C++23, and `icx-cl` names it
  the Microsoft way.
- `set "PATH=%PATH%;...Git\usr\bin"` appends Git's tools at the end, so they
  supply `xxd` without shadowing anything else.

The build needs `ocloc` for the default ahead-of-time targets
([AOT targets](overview.md#aot-targets-default-adr-0568)); on Windows it comes
with oneAPI (`where ocloc`). An older `ocloc` may not know every default
target. oneAPI 2025.1.1's rejects `bmg-g31` with `Unknown acronym`; check
with `ocloc ids <target>` and drop the unknown ones:

```bat
meson configure build -Dsycl_icpx_aot_targets=dg2-g10,dg2-g11,acm-g10,acm-g11,acm-g12,tgllp,adl-s,adl-p,adl-n,rpl-s,rpl-p,mtl-h,mtl-u,arl-h,arl-s,arl-u,lnl-m,bmg-g21
```

`-Dsycl_icpx_aot_targets=` (empty) builds SPIR-V only: every run then
compiles the kernels for the GPU at startup.

### How the kernels reach the program

Meson links MSVC builds with `link.exe` directly, and `link.exe` does not
know the SYCL device code in the compiled objects. So the build runs one
extra step, `icpx -fsycl -fsycl-link`, over all SYCL objects. That step
compiles the device code for every AOT target (about two minutes for 18
targets on a 16-core desktop) and writes `sycl_device_link.obj`, which
registers the kernels when the program starts. The object is part of
`vmaf.lib`, and any program that uses the SYCL backend links it
automatically, including an FFmpeg built against the static library
([ADR-1364](../../adr/1364-windows-sycl-msvc-device-link.md)).

## Run

List the GPUs the SYCL runtime sees, in the same environment:

```bat
sycl-ls
```

```text
[level_zero:gpu][level_zero:0] Intel(R) oneAPI Unified Runtime over Level-Zero, Intel(R) Arc(TM) B580 Graphics 20.1.0
[level_zero:gpu][level_zero:1] Intel(R) oneAPI Unified Runtime over Level-Zero, Intel(R) UHD Graphics 770 12.2.0
```

Pick a GPU with `ONEAPI_DEVICE_SELECTOR` (or `--sycl_device N`) and score:

```bat
set ONEAPI_DEVICE_SELECTOR=level_zero:0
build\tools\vmaf.exe -r ref.yuv -d dis.yuv -w 576 -h 324 -p 420 -b 8 --backend sycl --json -o out.json
```

`--backend sycl` fails with exit code 100 when SYCL cannot start, instead of
falling back to the CPU; the log line `SYCL: using device: ...` names the GPU
that ran.

## Test

Run the suites through the repository runner; on Windows it waits for Meson
and returns its status:

```bat
python scripts\ci\run_meson_test.py -- -C build --suite sycl --print-errorlogs
python scripts\ci\run_meson_test.py -- -C build --suite fast --print-errorlogs
```

`test_sycl_kernel_registration` needs no GPU; it fails when a program was
linked without its device images. For CPU-versus-GPU parity of every SYCL
extractor, run
`python scripts\ci\cross_backend_parity_gate.py --vmaf-binary build\tools\vmaf.exe --backends cpu sycl ...`
([cross-backend gate](../../development/cross-backend-gate.md)).

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `SYCL exception ... No kernel named ... was found`, then `problem reading pictures` | the program was linked without `sycl_device_link.obj` | rebuild from a tree that has ADR-1364; `test_sycl_kernel_registration` shows the count |
| `setvars.bat` prints `"vars.bat" is not recognized` for every component | `NoDefaultCurrentDirectoryInExePath` is set in the environment, so `cmd` does not run scripts from the current directory | `set NoDefaultCurrentDirectoryInExePath=` before `setvars.bat` |
| `MSB8040: Spectre-mitigated libraries are required` while building Level Zero | Visual Studio generator with the loader's `/Qspectre` | use `cmake -G Ninja`, or install the Spectre libraries |
| `Unknown acronym <target>` from `ocloc` | this oneAPI's `ocloc` predates the target | narrow `-Dsycl_icpx_aot_targets` |
| `Program xxd found: NO` at configure, then `--model version=...` fails | no `xxd` on `PATH` | add Git for Windows' `usr\bin` at the end of `PATH` |
| `test_registry` / `test_cli` fail with `/bin/bash: C:tmp...: No such file or directory` | Meson picked `System32\bash.exe`, the WSL launcher | fixed: the `dnn` tests use Git for Windows' bash, or are skipped without it |
