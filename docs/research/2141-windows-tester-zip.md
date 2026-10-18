<!-- markdownlint-disable MD013 MD060 -->
# Research-2141: Windows tester zip

- **Status**: Active
- **Workstream**: [ADR-1515](../adr/1515-windows-tester-zip.md)
- **Last updated**: 2026-10-04

What the Windows tester zip is built from, what ships in it and under which terms,
what was measured without a Windows machine, and what only the hosted runners and a
tester's machine can prove.

## What the hosted Windows lanes already show

Read from run 37163432688 of `libvmaf-build-matrix.yml` (2026-10-04), jobs
`Windows MSVC+CUDA`, `Windows ARM64 MSVC` and `Windows MSVC+SYCL`:

| Fact | Value |
| :--- | :--- |
| Visual Studio on `windows-2025` and `windows-11-vs2026-arm` | Visual Studio 2026 Enterprise (`Microsoft Visual Studio\18\Enterprise`), MSVC 19.51.36260, `link` 14.51.36260 |
| Windows SDK | 10.0.26100.0 |
| `nasm` (x64 only) | `C:\Strawberry\c\bin\nasm.exe`, found by Meson |
| `xxd` (embeds the models) | `C:\Program Files\Git\usr\bin\xxd.exe`, found by Meson on both architectures |
| Runtime library of the MSVC lanes | `/MD` (Meson's default for a release build) |
| `Windows ARM64 MSVC`, `fast` suite | 267 passed, 0 failed, 1 skipped |
| `Windows MSVC+CUDA (full)` (build.yml) | 16 CPU unit tests; no x86 SIMD unit test runs on an MSVC build anywhere |

`cl.exe` ignores the `-mavx512*` arguments of the AVX-512 libraries (warning D9002):
MSVC compiles AVX-512 intrinsics without an architecture switch, so the AVX-512 paths
are built. Whether they return the scalar's values has never been measured on MSVC;
the zip's report on the hosted runner (whose CPU may or may not have AVX-512) and on a
tester's machine are the first measurements.

## Licences of what ships

Read on 2026-10-04 unless stated.

| Component | Files | Terms | Source of the terms |
| :--- | :--- | :--- | :--- |
| VMAFx | `build\tools\vmaf.exe`, `tests\*.exe`, the report program | EUPL-1.2 and the inherited licences, computed per compiled file (`licensing.py scan-build`) | the files' SPDX headers and `REUSE.toml` |
| Microsoft C and C++ runtime, statically linked | inside every VMAFx program (`/MT`) | Visual Studio licence; the STL is Apache-2.0 WITH LLVM-exception | [Choose a deployment method](https://learn.microsoft.com/en-us/cpp/windows/choosing-a-deployment-method) (2022-06-28): static linking is a way to redistribute the Visual C++ libraries, discouraged for servicing; [Universal CRT deployment](https://learn.microsoft.com/en-us/cpp/windows/universal-crt-deployment) (2022-02-07): "Statically link the Universal CRT" is a listed deployment, and the UCRT is part of Windows 10 and later; [microsoft/STL](https://github.com/microsoft/STL) licence |
| `vcruntime140.dll`, `vcruntime140_1.dll` (interpreter) | `runtime\` | Microsoft Distributable Code | [Visual Studio 2026 redistribution list](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution) (ms.date 2025-11-11): "you may copy and distribute with your program any of the files within the following folder and its subfolders ... [VisualStudioFolder]\VC\redist", never `debug_nonredist`, "unmodified form" |
| CPython 3.13.16 | `runtime\` | PSF-2.0, `Doc/license.rst` (pinned SHA-256), HACL\* MIT | [python-build-standalone licensing](https://gregoryszorc.com/docs/python-build-standalone/main/running.html), the `full` archive's `python/licenses/` |
| Libraries python-build-standalone ships on Windows | `runtime\DLLs\` (`libcrypto-3-x64.dll`, `libssl-3-x64.dll`, `libffi-8.dll`, `sqlite3.dll`) and inside `python313.dll` and the `.pyd` modules | OpenSSL Apache-2.0, libffi MIT, SQLite public domain, expat MIT, mpdecimal BSD-2-Clause, bzip2, liblzma 0BSD, zlib | `PYTHON.json` and `python/licenses/` of `cpython-3.13.16+20261001-*-pc-windows-msvc-pgo-full.tar.zst` |
| Netflix test videos | `python\test\resource\` | BSD-2-Clause-Patent | `Netflix/vmaf_resource` `LICENSE` at the pinned commit |

Nothing copyleft ships: the zip has no source companion.

The Visual Studio 2026 licence terms were first read only in outline: the page
`visualstudio.microsoft.com/license-terms/vs2026-ga-pro-enterprise/` serves a title and a
date. A headless Chrome render on 2026-10-04 showed why: the page embeds a Word document
in Office's web viewer. The English document,
`wp-content/uploads/2025/10/Visual_Studio_2026-License-Enterprise_Professional_ENU.docx`
(67,523 bytes, SHA-256 `dd2ab92a7c2bb90dd509e16572707a34b13bc0bc09b9199d115f25594ac39053`,
Last-Modified 2025-10-31), is "MICROSOFT SOFTWARE LICENSE TERMS / MICROSOFT VISUAL STUDIO
ENTERPRISE 2026, VISUAL STUDIO PROFESSIONAL 2026, AND TRIAL EDITION / Last Updated:
October 1, 2025". Its DISTRIBUTABLE CODE section:

- lets a licensee "copy and distribute the object code form of code listed on the
  Distributable List located at `https://aka.ms/vs/18/redistribution`" (the list cited
  above, which names the files of `VC\redist`, unmodified, never `debug_nonredist`);
- requires the distributor to "add significant primary functionality to it in your
  applications", to "require distributors and external end users to agree to terms that
  protect the Distributable Code at least as much as these license terms", and to
  "indemnify, defend, and hold harmless Microsoft from any claims ... related to the
  distribution or use of your applications, except to the extent that any claim is based
  solely on the Distributable Code";
- forbids using "Microsoft's trademarks in your applications' names or in a way that
  suggests your applications come from or are endorsed by Microsoft".

The list does not name the static runtime libraries; Microsoft documents static linking
as a deployment of them (the deployment pages above), and the record applies the same
conditions to that code. Both Microsoft components now carry the document's text
(`fetched_texts`, extracted from the pinned `.docx` by `licensing.py fetch-texts`), and
their notes pass on the protective terms. The indemnity is an obligation of whoever
publishes the zips, not a term for testers; it binds every Windows program that ships
Visual C++ runtime code, and publishing the zips accepts it.

## The interpreter

python-build-standalone 20261001, the release the macOS bundle pins:

| Archive | SHA-256 (from the release's `SHA256SUMS`) |
| :--- | :--- |
| `cpython-3.13.16+20261001-x86_64-pc-windows-msvc-install_only_stripped.tar.gz` | `c402eb9a35aba90de319bbc87d23e6d887eca1551a05cfa80775f92dcf3ebc08` |
| `cpython-3.13.16+20261001-aarch64-pc-windows-msvc-install_only_stripped.tar.gz` | `0c5f9dedc4302b74600046a905f36a654a0710c1940bed202a4644efcba5e86f` |
| `cpython-3.13.16+20261001-x86_64-pc-windows-msvc-pgo-full.tar.zst` | `fbccab8d1487f327874956eb2d0e979612c133f3619b6e86e4ec250cb0191b55` |
| `cpython-3.13.16+20261001-aarch64-pc-windows-msvc-pgo-full.tar.zst` | `d88129f3600d12887a9fee50847d71eac7127b9be20785fa759e4459b9c1bee8` |

The x64 `install_only_stripped` archive unpacks to 68 MB and ships its own
`vcruntime140.dll` and `vcruntime140_1.dll` (file version 14.44.35211.0, Visual Studio
2022). The import tables of every program in it, read with
`scripts/ci/check-windows-bundle-imports.py`'s parser: `python.exe`, `python313.dll`
and every `.pyd` import `vcruntime140.dll` and the Universal CRT API sets; only
`_wmi.pyd` imports `vcruntime140_1.dll`; `zlib1.dll` (imports the legacy
`msvcrt.dll`) is used only by `tcl86t.dll`; pip's launchers include x86 and Arm64
programs. Pruned as the build does (headers, import libraries, pip, Tcl/Tk with
`zlib1.dll`, test modules, IDLE, `pythonw.exe`) the interpreter is 31 MB in 582 files,
and every remaining import is a Windows DLL or a file of `runtime\`.

## Host facts without `/proc/cpuinfo`

`IsProcessorFeaturePresent` (Microsoft Learn, updated 2025-12-18) reports SSE2 (10),
SSE3 (13), SSSE3 (36), SSE4.1 (37), SSE4.2 (38), AVX (39), AVX2 (40), AVX512F (41) and,
from Windows 11 24H2, BMI2 (60) and the Arm SVE family (46 onwards); SSSE3 to AVX512F
need Windows 10 2004. It has no bit for AVX-512 CD, BW, DQ or VL, which
`core/src/x86/cpu.c` also requires: the report reads AVX-512 from AVX512F and says so
in `dispatch_flags_source` (the processors with AVX512F but without BW, DQ and VL are
Xeon Phi parts). The processor's name, vendor and family come from
`HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0` (`ProcessorNameString`,
`VendorIdentifier`, `Identifier`), mapped to the keys of the existing allow-list.

## What was run here, without Windows

- The report path under Wine 11.19 on a reference workstation: a MinGW-w64 (GCC 16.2) cross
  build of `vmaf.exe` and the 51 tests of the Windows list it has, staged as the zip
  is (relative test paths), with the pruned x64 interpreter above. Under Wine,
  `hw_winfacts` read the processor from the registry and the features from
  `IsProcessorFeaturePresent` (`AMD Ryzen 9 9950X3D`, flags `sse2 ssse3 sse4.1 avx2
  avx512`); `generate-reference` wrote both references; `wine cmd /d /c run.cmd` ran
  the report end to end in 55 s and wrote `report.json` through `--output`: dispatch
  equivalence identical, reference equivalence identical, 51 of 51 unit tests passed,
  verdict `pass`. `check-hardware-reports.py --report` accepts the schema and refuses
  the report only because a local build is not the hosted workflow's. This proves the
  Python side (registry and kernel32 calls, the bounded runner on Windows, paths,
  the launcher); it says nothing about the MSVC build.
- The import check on that MinGW tree finds only the Universal CRT imports of the
  MinGW programs (a `/MD`-like build), which is the finding it exists for; the pruned
  interpreter passes.
- The `windows-zip` licence record against a tree of that shape with the real
  interpreter: notices written, check passes; with a Microsoft DLL planted in `tests\`
  and `runtime\LICENSE.txt` removed, the check fails on both.
- Measured sizes of the MinGW tree: tests 548 MB (about 11 MB per program: static
  libvmaf, the built-in models, symbols), `vmaf.exe` 16 MB, interpreter 31 MB,
  fixtures 56 MB. MSVC programs carry no symbols in the executable; the zip's real
  size comes from the hosted build.

## The CUDA zip (ADR-1516)

- `libvmaf` loads the driver at run time through nv-codec-headers'
  `dynlink_loader.h`, which opens `nvcuda.dll` on Windows; no program of the build
  imports an NVIDIA DLL, and the import check makes that a build failure. The CUDA
  runtime (`cudart64_13.dll`), which Attachment A would allow, is not linked and does
  not ship.
- The toolkit on the runner comes from NVIDIA's Windows redistributable archives
  (`scripts/ci/install-cuda-toolkit.ps1`, `redistrib_13.4.2.json`). Every archive holds
  the same `LICENSE` (68,070 bytes, MD5 `a3c52c1f3e8953cbfbc50639ad4b63e1` for
  `cuda_crt` and `visual_studio_integration` 13.4.92): the CUDA Toolkit EULA, "Last
  updated: January 26, 2026", which lists `libdevice.10.bc` in Attachment A. The build
  copies the installed copy after checking both strings, as the Linux image checks the
  Debian copyright file.
- `-fsycl` cannot be combined with `/MT` on Windows (Intel oneAPI DPC++/C++ Compiler
  guide 2025.2, `-fsycl`: "On Windows, option -fsycl sets option /MD ... You cannot
  specify option /MT"); a Windows SYCL zip would need the Visual C++ runtime DLLs next
  to the VMAFx programs and is left to its own decision.
- Not proven before a tester runs it: the CUDA probe under Windows (the driver calls
  are the Linux kit's, measured on the RTX 4090; the DLL name differs) and every CUDA
  kernel of the MSVC build on a GPU. The hosted run shows the build, the licence gate
  and `no_device`.

## The SYCL zip (ADR-1566)

Read on 2026-10-04 from the oneAPI Base Toolkit 2025.3.0.372 offline installer for
Windows, the pinned release of the `Windows MSVC+SYCL` lane (2,691,346,600 bytes,
SHA-256 `f4dde6e5ea732b1624f0a50df546401712bccdf45766c9a23b0865f95863f541`, downloaded
into memory, not installed). The installer is a stub with an appended zip; each
component is a `cupPayload.cup` (zip, LZMA) with a `manifest.json` naming its required
packages.

- `intel.oneapi.win.cpp-dpcpp-common` (the component the lane installs) requires
  `compilers-common` and `cpp-base-pro-common-runtime`, which require
  `compilers-common-runtime`, which requires `umf` (1.0.2), which requires `tcm`
  (1.4.1). The install of that one component therefore brings every runtime file the
  zip ships.
- Files under `_installdir/`: `compiler/2025.3/bin/` holds `sycl8.dll`,
  `ur_win_proxy_loader.dll`, `ur_loader.dll`, `ur_adapter_level_zero.dll`,
  `ur_adapter_level_zero_v2.dll`, `libmmd.dll`, `svml_dispmd.dll`, `libircmd.dll`,
  `libirngmd.dll`; `umf/1.0/bin/umf.dll`; `tcm/1.4/bin/libhwloc-15.dll`. The
  installer's post-install script creates the `latest` link of each
  (`New-Item -ItemType SymbolicLink -Path latest`).
- `compiler/2025.3/share/doc/compiler/credist.txt` lists the Windows redistributables
  as `<installdir>/bin/<name>` (all of the DLLs above, `umf.dll` and
  `libhwloc-15.dll` included); its header names the Intel End User License Agreement
  for Developer Tools. `licensing/c/LICENSE.rtf` is that agreement, "Version August
  2024", as on Linux; `third-party-programs.txt` sits beside it.
- Imports, read with the zip's own PE parser from the Windows wheels of the same
  release line (2025.3.1 on PyPI): `sycl8.dll` imports `libmmd.dll`,
  `ur_win_proxy_loader.dll` and the Visual C++ runtime; both Level Zero adapters
  import `umf.dll`, `libmmd.dll` and the runtime, and load `ze_loader.dll` by name;
  `umf.dll` imports `libhwloc-15.dll`. Nothing imports `ur_loader.dll` or the adapters:
  the SYCL runtime loads them by name, which is why the import check accepts exactly
  those names as loaded at run time.
- The Level Zero loader v1.34.0 builds with MSVC's static runtime by default
  (`MSVC_BUILD_L0_DYNAMIC_VCRUNTIME` off); its only third-party code (`third_party/xla`,
  Apache-2.0) goes into the validation layer, which the zip does not ship.

`test_sycl_kernel_scratch` with `VMAF_SYCL_SCRATCH_RATCHET_FILE`, run on the project's
Arc A380 (Linux, oneAPI 2026.0, a JIT-only build): unset, set to a copy of the list and
set empty, it audits 131 kernels and passes; set to a missing file it fails with
"cannot open core/src/sycl/scratch_ratchet.txt".

## The first hosted run (run 37170921097, 2026-10-04)

A `publish: false` dispatch on master `010140154` (the merge of the zip's pull request):

| | x64 (`windows-2025`) | arm64 (`windows-11-vs2026-arm`) |
| :--- | :--- | :--- |
| Build (MSVC 19.51.36260, `/MT`, 51 tests) | passed, 1 min 47 s; the whole job about 5 min | passed |
| References, notices, licence texts (`compression.zstd` of Python 3.14.8 read the `full` archive), `scan-build` | passed | passed |
| Import check | passed: no VMAFx program imports a runtime DLL | failed: `runtime/vcruntime140_1.dll` is machine 0x8664 |
| The zip's own report through `run.cmd` (98 s) | verdict `fail`: dispatch equivalence identical on all four fixtures, reference equivalence identical, 50 of 51 unit tests, `test_float_adm_x86` failed | not reached |
| Licence gate, pack | passed; 46,453,578 bytes, 726 files, 127.1 MB unpacked (test videos 58.2 MB, tests 34.3 MB, interpreter 31.1 MB, `vmaf.exe` 2.8 MB) | not reached |

The runner's processor was an AMD EPYC 7763 (`sse2 ssse3 sse4.1 avx2`, no AVX-512), Windows
Server 2025 10.0.26100, image `win25-vs2026-20260925.250.1`. The report passed
`check-hardware-reports.py --report` (run here on the downloaded artifact; the workflow's
verify job was skipped because the Arm64 leg failed). Both findings are state rows:
`T-TESTER-WINDOWS-ARM64-X64-VCRUNTIME-2026-10-04` (the Arm64 interpreter archive's x64
`vcruntime140_1.dll`, which nothing imports, fixed by removing unimported runtime DLLs)
and `T-MSVC-FLOAT-ADM-X86-TEST-FAILS-2026-10-04` (open; the build log now prints a failed
test's output so the next run names the case).

## The second hosted run (run 37178704550, 2026-10-04)

A `publish: false` dispatch on master `e00c17bc8`, after the Arm64 fix and with the CUDA
leg ([ADR-1516](../adr/1516-windows-cuda-tester-zip.md)) for the first time:

| | x64 | arm64 | x64-cuda |
| :--- | :--- | :--- | :--- |
| Build job | passed, 6 min 39 s | passed, 7 min 37 s | passed, 9 min 47 s (CUDA 13.4.92 archives) |
| Report verdict | `fail`: 50 of 51 unit tests | `pass`: 39 of 39 unit tests | `fail`: 50 of 51 unit tests; GPU `no_device`, reason names `nvcuda.dll` |
| Dispatch and reference equivalence | identical (AMD EPYC 7763, AVX2) | identical (Cobalt 100, NEON) | identical (AMD EPYC 7763, AVX2) |
| Zip | 46,454,287 bytes, 726 files, 127.1 MB unpacked | 40,865,508 bytes, 713 files, 118.6 MB unpacked | 350,462,085 bytes, 933 files, 1,057.2 MB unpacked |

All three reports pass `check-hardware-reports.py --report` (run on the downloaded
artifacts; a report whose verdict is edited to `pass` is refused). The x64 failure is
`test_dwt2_avx2_matches_scalar_on_signed_zeros`: MSVC removes the intrinsic
`_mm256_add_ps(_mm256_setzero_ps(), p)` that starts the AVX2 wavelet sum, while it keeps
the scalar `accum = 0; accum += p`, so a sample whose four products are all -0 comes out
-0 against the scalar's +0. MSVC 19.51 x64 on Compiler Explorer, `/O2 /fp:precise
/arch:AVX2`, shows both shapes; it also folds an intrinsic `x - (+0)` and keeps
`x + (-0)`, `x - (-0)` and a compare-and-mask. Microsoft's `/fp` page says `/fp:precise`
processes -0.0 according to IEEE-754 and says nothing about intrinsics. GCC and Clang keep
the addition, so the MSVC lanes are the only ones that see it; the fix forms `+0 + p` with
a compare and a mask (`T-MSVC-FLOAT-ADM-X86-TEST-FAILS-2026-10-04`). Of the two forms
MSVC keeps, the compare and the mask is the one whose value follows from IEEE-754 alone:
`x - (-0)` equals `+0 + x` too, but survives only as long as MSVC's folding keys on an
all-zero constant. The AVX-512 kernel gets the same form, although no hosted run has
reached it.

In the CUDA zip, 952.4 MB of the 1,057.2 MB are its 114 test programs: the 73 that link
libvmaf carry their own static copy with every CUDA image (12.7 to 13.9 MB each, 941.6 MB
together; the largest test of the CPU zip is 2.7 MB). That is the `/MT`, static-library
design of [ADR-1515](../adr/1515-windows-tester-zip.md) applied to a CUDA build. Linking
the tests against one shared library would keep a single copy of those images; it is not
decided here.

## Found on the way

The macOS bundle published as `tester-20261003-c12763f3` lists its unit tests by their
absolute path on the hosted runner, so no unit test starts on a tester's Mac
(`T-TESTER-BUNDLE-UNIT-PATHS-ABSOLUTE-2026-10-04`, fixed in a separate pull request;
the Windows zip depends on that fix).

## Not proven before the hosted run

- That the MSVC build links cleanly with `-Db_vscrt=mt` (no lane has used it).
- Which unit tests of the x86 SIMD list fail on MSVC, and whether the hosted x64
  runner has AVX-512. Answered by the two runs: only the AVX2 signed-zero case of
  `test_float_adm_x86`; both x64 runners were an AMD EPYC 7763 without AVX-512, so the
  MSVC build of the AVX-512 kernels has not run.
- `compression.zstd` in the runner's Python 3.14.8 on both architectures (the build
  falls back to a `zstd` program on PATH and fails with a message without either).
  Answered: it read the archive on both.
- The Arm64 redistributable folder holding both `vcruntime140.dll` and
  `vcruntime140_1.dll` (the build fails with the missing name if not). Answered: the
  second run's Arm64 build passed.
- Anything on a tester's machine: SmartScreen and Smart App Control behaviour, and the
  scores of his processor.
