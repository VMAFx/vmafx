<!-- markdownlint-disable MD013 MD060 -->
# ADR-1566: A Windows SYCL tester zip built with /MD that carries its runtime beside every program and measures every SYCL twin on a tester's Intel GPU

- **Status**: Accepted; superseded in part by [ADR-3567](3567-windows-sycl-oneapi-2026-2.md) on 2026-11-19: the CI lane pin changed; the tester zip pin stays.
- **Date**: 2026-10-04
- **Deciders**: maintainer, agent
- **Tags**: ci, release, windows, sycl, testing, parity, license, fork-local

## Context

The Windows CUDA zip (ADR-1516) left a Windows SYCL zip to its own decision. The fork's
Windows SYCL build exists: the `Windows MSVC+SYCL` lane builds it with `icx-cl` and the
explicit device link of ADR-1364, but it only checks that the device images register;
its runner has no GPU, and the project's Intel GPUs run Linux. So no SYCL kernel of a
Windows build has ever run.

Three facts shape the zip. Intel's compiler refuses the static runtime with `-fsycl`
("On Windows, option -fsycl sets option /MD ... You cannot specify option /MT", compiler
guide 2025.2), which ADR-1503 rule 7 and the other Windows zips prefer. libvmaf links the
Level Zero loader (`find_library('ze_loader')`), and the oneAPI installer for Windows
carries no `ze_loader.lib`, so the lane builds the loader from source. And Windows loads
a DLL a program imports from the program's own directory first, then System32, and only
then the directories on PATH: a Visual C++ runtime DLL in System32, which may be older
than the one the build needs, wins over a copy on PATH.

The maintainer decided for the zip (popup of 2026-10-04): built with `/MD`, shipping the
redistributable Visual C++ runtime and only Intel's `credist.txt`-listed SYCL runtime
DLLs, as the Linux Intel GPU image ships only listed files (ADR-1505), under the same
licence gate and report path, with the report's SYCL backend reaching the GPU through
Level Zero and the Windows graphics driver. The Visual Studio 2026 licence terms that
govern the runtime DLLs were read for this decision (Research-2141, "Licences of what
ships").

## Decision

We add a fourth zip to `.github/workflows/windows-tester-bundle.yml`,
`vmafx-tester-windows-x64-sycl-<describe>.zip`, built on `windows-2025` inside oneAPI's
`setvars`: `icx-cl` for C and C++ with `-Denable_sycl=true -Db_vscrt=md
-Dcpp_std=c++latest` and the default AOT target list, static libvmaf, the unit tests of
the other zips plus the Linux SYCL kit's device tests (`sycl-tests.txt`), parity gate,
twin bounds and row map (`sycl-rows.json`); x64 only.

- **Compiler**: the oneAPI Base Toolkit 2025.3.0.372 offline installer, the release of
  the `Windows MSVC+SYCL` lane, pinned by URL, size (2,691,346,600 bytes) and SHA-256
  (`f4dde6e5...`, measured 2026-10-04); only its DPC++/C++ component is installed, and
  its required packages bring the runtime, UMF 1.0.2 and TCM 1.4.1. It lags the Linux
  image's oneAPI 2026.1 as the lane does.
- **Everything a program loads lies in its own directory** (`build/tools/`, `tests/`):
  the Visual C++ runtime DLLs the programs and Intel's DLLs import, copied unmodified
  from the runner's redistributable folder; Intel's SYCL runtime from
  `tools/rc1-tester/image/sycl-runtime-windows.json`, every compiler file listed in the
  installed `credist.txt`, minus each listed DLL nothing imports and the runtime does
  not load by name; and the Level Zero loader built from `LEVEL_ZERO_VERSION` with
  MSVC's default static runtime. `check-windows-bundle-imports.py --runtime md` fails
  the build when an import resolves to neither Windows nor the program's directory, or
  when a DLL there is used by nothing.
- **Licences**: artifact `windows-sycl-zip`. It shares the interpreter, fixtures and
  record components with `windows-zip` and adds `microsoft-linked-runtime` (the start-up
  code of the `/MD` import libraries and the loader's static runtime),
  `microsoft-vc-runtime-programs`, the Intel components of the Linux image in their
  Windows form (the EULA as `LICENSE.rtf`, `third-party-programs.txt`, `credist.txt`;
  UMF; hwloc of TCM) and `level-zero-loader` (MIT). Every Microsoft component carries
  the Visual Studio 2026 licence terms.
- **Report**: `hw_sycl.py` on Windows opens the zip's own loader (`tests/ze_loader.dll`,
  passed to `hw_l0probe.py` in `VMAFX_ZE_LOADER`), records whether System32 holds a
  driver's loader, reads the oneAPI and loader versions from `image/gpu-runtime.json`,
  and names the Intel graphics driver when Level Zero finds no GPU. The hosted runner
  has no Intel GPU, so the verify job requires `gpu.status` `no_device` on path
  `windows`.
- **Scratch audit**: `test_sycl_kernel_scratch` reads its ratchet list from
  `VMAF_SYCL_SCRATCH_RATCHET_FILE` when set, since the path compiled into it names the
  runner's checkout; the zip carries the list and its manifest entry sets the variable.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| The DLLs in one `dll\` folder on PATH | One copy of each DLL | System32 is searched before PATH: an older `msvcp140.dll` there (from any program's redistributable) would be loaded, and a driver's `ze_loader.dll` would replace the zip's | Beside every program; the copies cost tens of MB unpacked |
| Expect the Visual C++ redistributable and the oneAPI runtime installed | Smaller zip | A tester installs software first; versions vary per PC | The kit's rule is one command, nothing installed |
| Rely on the Intel driver's `ze_loader.dll` in System32 | No loader to build | libvmaf imports the loader: without an Intel driver no program starts, not even the CPU checks or the hosted run | The zip's loader; the driver's Level Zero driver still serves the GPU |
| Build the CPU code with MSVC and only the SYCL files with icx | The CPU checks would measure the MSVC build | The build system compiles a SYCL build with one toolchain (ADR-1364); a mixed build is a new build path to maintain | The guide says the SYCL zip's CPU checks measure Intel's build |
| oneAPI 2026.1, the Linux image's release | One compiler version across kits | Not proven on Windows by any lane; its installer is not pinned anywhere yet | The lane's proven release; a bump moves both together |
| A fixed DLL list without pruning | Simpler build | A wrong guess ships an unused DLL (ADR-1503 rule 1) or misses one, found only by a hosted run hours later | The spec lists candidates, the build prunes and records |

## Consequences

- **Positive**: one command runs every SYCL twin of the Windows build against the CPU,
  the gate, the device tests and the scratch audit on a tester's Intel GPU; the first
  evidence that the Windows SYCL build works on a GPU, and the first Xe-LP reports from
  Windows PCs.
- **Negative**: nothing proves the zip on a GPU before a tester runs it; the hosted run
  shows the build, the licence gate and `no_device` only. The zip carries the AOT images
  of the default target list in every device test program and its runtime DLLs twice.
  Its CPU checks measure the `icx-cl` build (ADR-1495's libimf policy is Linux only).
  Distributing the Visual C++ runtime DLLs accepts the Distributable Code terms of the
  Visual Studio licence, including the distributor's indemnity of Microsoft.
- **Neutral / follow-ups**: the state row `T-SYCL-WINDOWS-BUILD-NEVER-RUN-ON-A-GPU-2026-10-04`
  closes with an accepted report; a new oneAPI installer changes the pins, the spec's
  names and the record's versions together.

## References

- `Q` (maintainer popup, 2026-10-04, relayed by the coordinator): "Yes, with the Intel
  runtime DLLs (Recommended)" — a fourth zip built with `/MD`, shipping the
  redistributable VC++ runtime and only Intel's credist-listed SYCL runtime DLLs, as
  the Linux SYCL image does, same licence gate and report path; the `gpu` section's SYCL
  backend on Windows through Level Zero and the Windows driver.
- `req` (coordinator, 2026-10-04, paraphrased): the zips must not rely on licence terms
  nobody read; read the Visual Studio terms another way and cite file and date.
- [ADR-1516](1516-windows-cuda-tester-zip.md), [ADR-1515](1515-windows-tester-zip.md),
  [ADR-1505](1505-intel-gpu-tester-image.md), [ADR-1503](1503-tester-artifact-licensing.md),
  [ADR-1364](1364-windows-sycl-msvc-device-link.md), [ADR-1395](1395-sycl-kernels-no-scratch.md),
  [Research-2141](../research/2141-windows-tester-zip.md).
- Intel oneAPI Base Toolkit 2025.3.0.372 offline installer for Windows: package
  manifests and `compiler/2025.3/share/doc/compiler/credist.txt` (Intel EULA for
  Developer Tools, Version August 2024 in `licensing/c/LICENSE.rtf`), read 2026-10-04.
