# Intel oneAPI install: local SYCL toolchain

Install the Intel oneAPI DPC++ compiler `icpx`, the `ocloc` offline compiler
and a Level Zero runtime, then configure a SYCL build. The SYCL backend
(`-Denable_sycl=true`) needs all three. This page covers the local developer
machine on Linux; for a native Windows build see
[SYCL on Windows](../backends/sycl/windows.md).

CI installs oneAPI from Intel's apt repository on Linux and from Intel's
offline installer on Windows; see [CI vs local](#ci-vs-local).

## Quick path

1. Install `ocloc`, the offline GPU compiler that the default
   ahead-of-time build calls
   ([details](#the-ocloc-offline-compiler)):

    ```bash
    bash scripts/ci/install-intel-ocloc.sh
    ```

2. Install the oneAPI compiler (see [Install paths](#install-paths)) and
   activate it for the shell:

    ```bash
    source /opt/intel/oneapi/setvars.sh
    icpx --version
    ```

3. Configure and build from the repository root:

    ```bash
    meson setup build-sycl core -Denable_sycl=true -Denable_cuda=false
    ninja -C build-sycl
    ```

For a SPIR-V build without `ocloc` (for example to run clang-tidy), configure
with `-Dsycl_icpx_aot_targets=`: the binary then compiles its kernels at first
launch instead.

## Pinned version

The pin lives in `build-config.env`; this page does not repeat the numbers
except where a command needs one.

| Component | Pin | Notes |
| --- | --- | --- |
| Intel oneAPI compiler (Linux) | `ONEAPI_VERSION` (2026.1), exact apt build `ONEAPI_APT_VERSION` | `icpx` ships with the compiler package; CI and the release image install it from Intel's apt repository. |
| Intel oneAPI (Windows) | `WINDOWS_BASEKIT_URL` in `libvmaf-build-matrix.yml` (2026.2.0.661, [ADR-3567](../adr/3567-windows-sycl-oneapi-2026-2.md)); `ONEAPI_WINDOWS_VERSION` (2025.3.0.372) and `INTEL_BASEKIT_WINDOWS_URL`, `_SIZE`, `_SHA256` in `build-config.env` (the installer the Windows SYCL tester zip checks and installs) | The offline installer URL carries a per-build GUID; `libvmaf-build-matrix.yml` pins 2026.2.0.661 (Clang 23 front end) for MSVC 14.52 `STL1000` compatibility. |
| Compute runtime (`level-zero-loader`) | distro package | Arch / CachyOS: `pacman -S level-zero-loader`. |
| `ocloc` (GPU offline compiler) | `INTEL_NEO_VERSION` | Needed for the default ahead-of-time build. |
| Level Zero loader (container) | `LEVEL_ZERO_VERSION` | Installed by `install-intel-ocloc.sh --components build`. |

## Building with icx / icpx

Any SYCL build uses `icpx`. You can also build the CPU code with Intel's
compilers (`icx` for C, `icpx` for C++); the `vmaf` of the dev container
(`dev/Containerfile`), the `Linux Intel LLVM` job of `build.yml` and the
`Ubuntu SYCL` and `Ubuntu SYCL+CUDA` legs of `libvmaf-build-matrix.yml` are
built that way.

1. Configure the host compilers through the environment, then build:

    ```bash
    CC=icx CXX=icpx meson setup build-icx core
    ninja -C build-icx
    ```

    A SYCL build (`-Denable_sycl=true`) takes `icpx` from `sycl_compiler`
    (default `icpx`) for the device code in addition.

2. Check the math library. The build passes `-no-intel-lib=libimf` to every
   C and C++ link, so the CPU extractors call glibc's `libm` and an icx
   build returns the scores of a GCC build
   ([ADR-1495](../adr/1495-icx-system-libm.md)). Nothing is needed on the
   command line; the block is in `core/src/meson.build` between the
   `BEGIN / END VMAF host math library link policy` markers. Verify it with:

    ```bash
    python3 scripts/ci/run_meson_test.py -- -C build-icx test_icx_system_libm
    ```

What an icx build does, and does not, change:

| Topic | Behaviour | Reference |
| --- | --- | --- |
| Floating-point flags | Every C and C++ translation unit gets `-fp-model=precise -ffp-contract=off`, so nothing fuses `a * b + c` on its own. | [ADR-1461](../adr/1461-strict-fp-every-translation-unit.md), [build flags](build-flags.md#floating-point-contraction-is-off-everywhere) |
| Math library | Linux links glibc `libm`, not Intel's `libimf`. | [ADR-1495](../adr/1495-icx-system-libm.md) |
| SYCL device code | Own strict list (`sycl_strict_fp_args`) on every kernel TU; device math libraries stay linked. | [ADR-1367](../adr/1367-sycl-strict-fp-every-feature-tu.md) |
| Netflix golden gate | Never built with icx: `make test-netflix-golden` uses `gcc` or `clang` in `core/build-golden`, because icx contraction drift moves float-motion and float-VIF scores. | [ADR-1317](../adr/1317-golden-gate-build-isolation.md), `scripts/ci/setup-golden-build.sh` |

!!! warning "Windows `icx-cl` is not covered"
    The host-math policy applies to the Unix icx driver only. Whether a
    Windows build with `icx-cl` links Intel's math library is unmeasured and
    explicitly deferred (state row `T-ICX-CL-WINDOWS-HOST-MATH-2026-10-03` in
    [state.md](../state.md)). The Windows lane is build-only, so no score from
    it is compared with anything today.

## The `ocloc` offline compiler

The default build compiles native Intel GPU code for every target in
`sycl_icpx_aot_targets`
([ADR-0568](../adr/0568-sycl-icpx-aot-targets-default.md)), and `icpx` hands
that work to Intel's `ocloc`. The Linux oneAPI compiler does not include it
(the Windows one does), so `meson setup -Denable_sycl=true` stops with an error
naming this page's remedy when `ocloc` is not on `PATH`
([ADR-1360](../adr/1360-sycl-aot-compile-time-device-codegen.md)).

On Debian or Ubuntu x86-64, install the release the rest of the tree pins:

```bash
bash scripts/ci/install-intel-ocloc.sh     # sudo is used when not root
ocloc query OCL_DRIVER_VERSION             # prints INTEL_NEO_VERSION
```

The script downloads `intel-ocloc` and the two Intel Graphics Compiler packages
it loads from the `intel/compute-runtime` release named by `INTEL_NEO_VERSION`
in `build-config.env`. It checks them against the release's published SHA-256
sums, installs them and proves that `ocloc` can compile a kernel. Set
`GITHUB_TOKEN` if the anonymous GitHub API limit is exhausted.

On Arch / CachyOS the `intel-compute-runtime` package ships `ocloc`. The
`vmaf-dev-mcp` container already has it.

`--components` widens the set to the rest of the pinned Intel GPU stack. The
oneAPI release image uses both wider sets
([ADR-1368](../adr/1368-oneapi-release-image-debian13.md)):

| `--components` | Installs | For |
| --- | --- | --- |
| `ocloc` (default) | `intel-ocloc` and the two IGC packages | a build host that already has a Level Zero loader |
| `build` | the `ocloc` set plus the Level Zero loader and its headers (`libze1`, `libze-dev`) at `LEVEL_ZERO_VERSION` | a build host with no other Level Zero source |
| `runtime` | the whole compute runtime (Level Zero GPU driver, OpenCL ICD, gmmlib, IGC, `ocloc`), the set `vmaf-dev-mcp` installs, plus `libze1` | a host or image that runs SYCL kernels on an Intel GPU |

The Level Zero loader release publishes no checksum file, so the script checks
those debs against the SHA-256 digests GitHub records for the release assets.

## Install paths

oneAPI installs under `/opt/intel/oneapi/`. Pick one way to get it:

| Option | Command | When |
| --- | --- | --- |
| Intel apt repository script | `scripts/ci/install-intel-oneapi.sh --mode=builder` | Debian or Ubuntu; installs the pinned build, as CI and the release image do |
| Intel offline installer | see [below](#intel-offline-installer) | Side-by-side with another version, for A/B compiler benchmarks |
| Arch / CachyOS package | `sudo pacman -S intel-oneapi-basekit` | One global install; the repository lags Intel by one or two releases |
| AUR | `paru -S intel-oneapi-basekit-2025` (or `yay -S`) | One global install, newer than the Arch repository |

The apt script installs the compiler (`--mode=builder`) or its SYCL runtime
(`--mode=runtime`) from Intel's apt repository at the exact build
`ONEAPI_APT_VERSION` in `build-config.env`. It trusts only the repository key
named by `INTEL_ONEAPI_APT_SIGNER_FINGERPRINT`. The release image runs it on
Debian 13; it works on any Debian or Ubuntu host.

The AUR package conflicts with the Arch repository package and installs to
`/opt/intel/oneapi/` (not version-suffixed), so it replaces any existing
install. Check the packaged version before relying on it: both Arch sources
can be older than the pin in `build-config.env`.

### Intel offline installer

The offline `.sh` installer is about 2.6 GB. Download it from
[Intel oneAPI Base Toolkit downloads][intel-baset-toolkit]; its URL carries a
per-build identifier, so copy it from that page. Install into a versioned
directory so it does not replace an existing install at `/opt/intel/oneapi/`:

```bash
VERSION=2026.1
sudo sh ./intel-oneapi-base-toolkit-offline.sh \
    --silent --eula accept --components all \
    --install-dir /opt/intel/oneapi-$VERSION
```

Activate it for a shell:

```bash
source /opt/intel/oneapi-$VERSION/setvars.sh
icpx --version
```

[intel-baset-toolkit]: https://www.intel.com/content/www/us/en/developer/tools/oneapi/base-toolkit-download.html

## Multi-version coexistence

A common situation: the Arch repository or AUR install at `/opt/intel/oneapi/`
is held back at an older release whose device images no longer match the
system's `level-zero-loader`, while a side-by-side install at
`/opt/intel/oneapi-<version>/` ships matching binaries. Run against the older
install, `vmaf_bench` silently falls back to the host or fails `ze_init`; the
symptom is "ran on CPU even though a GPU is plugged in".

The bench and lint cycle has to point at the install whose runtime matches the
loader. The helper `scripts/ci/sycl-bench-env.sh` resolves the right
`setvars.sh` and emits an `eval`-able environment block:

```bash
eval "$(scripts/ci/sycl-bench-env.sh 2026.1)"
icpx --version
```

List the GPU devices the benchmark sees, then run it on one by index:

```bash
./build-sycl/tools/vmaf_bench --list-devices
./build-sycl/tools/vmaf_bench --device 0
```

The helper looks, in order, for:

1. `$ONEAPI_PREFIX` (explicit override);
2. `/opt/intel/oneapi-<version>/` (canonical side-by-side layout);
3. `/opt/intel/oneapi/<version>/` (Intel modulefile-style layout);
4. `/opt/intel/oneapi/` (fallback; warns if the requested version does not
   match the install actually present).

If none resolves, it exits 1 with a pointer back to this page. It forwards only
`CMPLR_ROOT`, `LD_LIBRARY_PATH`, `LIBRARY_PATH` and `PATH`, the four variables
`vmaf_bench`, `icpx` and `clang-tidy` consume, so the parent shell does not
accrete the roughly 40 variables `setvars.sh` exports.

## Verify a new compiler

After installing a new oneAPI version:

1. Activate it and force a clean SYCL build; the `icpx` version is baked into
   `compile_commands.json`:

    ```bash
    source /opt/intel/oneapi-2026.1/setvars.sh
    rm -rf core/build-sycl-lint
    meson setup core/build-sycl-lint core -Denable_sycl=true -Denable_cuda=false
    ninja -C core/build-sycl-lint
    ```

2. Check that the SYCL kernels still link:

    ```bash
    ls -la core/build-sycl-lint/src/libvmaf.so.3.0.0
    ```

3. Check that the host math functions still come from glibc
   ([ADR-1495](../adr/1495-icx-system-libm.md)). A new driver that renamed or
   dropped `-no-intel-lib=libimf`, or linked Intel's math library another way,
   would bring `libimf` back, and this test fails then:

    ```bash
    python3 scripts/ci/run_meson_test.py -- -C core/build-sycl-lint \
        test_icx_system_libm --print-errorlogs
    ```

    The test reads `libvmaf.so` and `vmaf` with `readelf` and runs
    `vmaf --version` under `LD_DEBUG=bindings`. The option is documented in
    `icx --help` ("Restrict linking of Intel specific libraries. Valid
    arguments are libirc, libimf, libirng, libsvml") and in the installed man
    page (`share/man/man1/icx.1`); check both on a bump.

## Verify SYCL clang-tidy

The icpx-aware wrapper `scripts/ci/clang-tidy-sycl.sh`
([ADR-0217](../adr/0217-sycl-toolchain-cleanup.md)) injects the oneAPI SYCL
include path and `__SYCL_DEVICE_ONLY__=0`, so stock LLVM `clang-tidy` resolves
`<sycl/sycl.hpp>` and SYCL translation units lint cleanly:

```bash
echo "core/src/sycl/picture_sycl.cpp
core/src/feature/sycl/integer_adm_sycl.cpp
core/src/feature/sycl/integer_motion_sycl.cpp
core/src/feature/sycl/integer_vif_sycl.cpp" \
  | parallel -j$(nproc) "scripts/ci/clang-tidy-sycl.sh \
      -p core/build-sycl-lint --quiet {}" \
  | grep -E "warning:|error:" \
  | wc -l
```

Expected: `0` across all four files.

If the wrapper cannot locate `<sycl/sycl.hpp>` automatically, point it at the
install:

```bash
ICPX_ROOT=/opt/intel/oneapi-2026.1/compiler/latest/linux \
  scripts/ci/clang-tidy-sycl.sh -p core/build-sycl-lint --quiet <file>
```

The wrapper also adapts an icx compile database to stock clang. It rewrites
`-fp-model=` to `-ffp-model=` in a copy of `compile_commands.json`, and passes
`-Wno-unknown-warning-option`, `-Wno-unknown-pragmas` and
`-Wno-overriding-option`. The last one is needed because a target that names
the strict floating-point arguments next to the project-wide ones compiles
with `-fp-model=precise -ffp-contract=off` twice; clang's driver comments on
the repeat with a warning that has no source location, and
`scripts/ci/tidy-ratchet.py` counts a warning it cannot place as a failed
translation unit. To check the wrapper after a toolchain change:

```bash
python3 -B -m unittest discover -s scripts/ci/tests -p 'test_tidy_ratchet.py'
```

## Upgrading oneAPI

After a major-version oneAPI bump, walk through these items before declaring
the bump complete. None block; each is a follow-up backlog candidate.

- [ ] `atomic_ref` performance: run the SYCL paths of `vmaf_bench`
  (`motion_sycl`, `adm_sycl`) on the canonical Arc or Battlemage host and
  compare per-frame timings against the previous version's numbers.
- [ ] `sub_group::shuffle_*` codegen: sample the IR for the VIF reduction loop
  in `integer_vif_sycl.cpp` and check whether the new compiler removes the
  `_mm`-style fallback written against older Arc generations.
- [ ] Sub-group sizes: kernels must declare 16 or 32 through
  `VmafSyclSubGroupSize`, never a raw `reqd_sub_group_size` attribute; check
  that the compiler still honours both sizes on every AOT target
  ([ADR-1468](../adr/1468-sycl-sub-group-sizes-every-aot-target.md)).
- [ ] `group_load` / `group_store` (2025.2+): a rewrite of the ADM DWT vertical
  and horizontal passes on
  `sycl::ext::oneapi::experimental::group_load` was sketched
  ([Research-0086 §A.4](../research/0086-sycl-toolchain-audit-2026-05-08.md)
  emitted GO) and then **deferred** under
  [ADR-0406](../adr/0406-sycl-adm-dwt-group-load-deferral.md): the
  `WG_SIZE × ElementsPerWorkItem` divisibility constraint and the multi-row
  source contiguity gap defeat it. It re-opens when a tile-geometry redesign
  yields integer divisibility and Xe2 or Battlemage hardware is available to
  confirm the register-pressure delta.
- [x] OpenVINO EP version bump: the ONNX Runtime bundled with the basekit
  exposes the NPU plugin through `device_type=NPU` on the existing
  `OpenVINOExecutionProvider`. Done 2026-05-08 in
  [ADR-0405](../adr/0405-openvino-npu-ep-wiring.md), which adds
  `--tiny-device=openvino-npu` (plus `openvino-cpu` and `openvino-gpu` for
  explicit device-type pinning). End-to-end NPU validation still needs a
  contributor with Meteor, Lunar or Arrow Lake hardware.
- [ ] C++23 surface: C++23 features (`std::expected`, `std::print`,
  `if consteval`) are usable with the current `icpx` but not adopted in any
  fork-local TU. Defer until a clear use case appears, likely the tiny-AI
  dispatch layer when the NPU EP lands.

## CI vs local

The Linux SYCL lanes install `${ONEAPI_APT_PACKAGE}` from Intel's apt
repository; its version is set once, as `ONEAPI_VERSION` in `build-config.env`,
and can differ from a local install. They install `ocloc` with
`scripts/ci/install-intel-ocloc.sh`. The `Windows MSVC+SYCL` lane pins its own
offline-installer release in the workflow.

Local-versus-CI divergence on the SYCL kernel binaries is acceptable as long as
both build cleanly and the `Ubuntu SYCL` matrix row stays green. Bit-identical
SYCL output is not a guaranteed invariant across Intel oneAPI releases.

## Related

- [ADR-0217](../adr/0217-sycl-toolchain-cleanup.md): SYCL toolchain
  consolidation; `icpx` is the chosen DPC++ implementation.
- [`docs/backends/sycl/overview.md`](../backends/sycl/overview.md):
  user-facing SYCL backend reference (`--sycl_device` and so on).
- [`docs/backends/sycl/bundling.md`](../backends/sycl/bundling.md): shipping a
  self-contained binary without an end-user oneAPI install.
- [`build-flags.md`](build-flags.md): the `enable_sycl` Meson option.
- [`sycl-toolchains.md`](sycl-toolchains.md): the AdaptiveCpp alternative.

## History

- **2026-04-25 (T7-8)**: pin bumped from oneAPI 2025.0.4 to 2025.3.1. The
  pin has since moved to 2026.1 (`build-config.env`).
- **2026-05-08**: OpenVINO NPU execution provider wired
  ([ADR-0405](../adr/0405-openvino-npu-ep-wiring.md)); the `group_load` rewrite
  deferred ([ADR-0406](../adr/0406-sycl-adm-dwt-group-load-deferral.md)).
