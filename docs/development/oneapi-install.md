# Intel oneAPI install — local SYCL toolchain

The fork's SYCL backend (`-Denable_sycl=true`) requires the Intel oneAPI
DPC++ compiler `icpx`. This page documents the minimum install needed for
the SYCL build + clang-tidy lint cycle, the version we pin against, and
the upgrade procedure when a newer Intel release ships.

CI installs oneAPI via the official `intel/oneapi-runtime-toolkit`
GitHub Action; this page covers the **local developer machine** path.

## Pinned version

| Component | Pinned version | Notes |
| --- | --- | --- |
| Intel oneAPI Base Toolkit | **2025.3.1** | Bumped from 2025.0.4 (2026-04-25, T7-8). |
| `icpx` (DPC++/C++ compiler) | shipped with the basekit | LLVM 20 base. |
| Compute runtime (`level-zero-loader`) | distro package | Arch / CachyOS: `pacman -S level-zero-loader`. |

## Install paths

oneAPI installs everything under `/opt/intel/oneapi/`. There are three
common ways to get a working install:

### 1. Official Intel offline installer (recommended for side-by-side)

Download the offline `.sh` installer from
[Intel oneAPI Base Toolkit downloads][intel-baset-toolkit]. The installer
is ~2.6 GB; sha256 matches the AUR PKGBUILD `b2sum` field for the
pinned version.

```bash
URL="https://registrationcenter-download.intel.com/akdlm/IRC_NAS/\
6caa93ca-e10a-4cc5-b210-68f385feea9e/\
intel-oneapi-base-toolkit-2025.3.1.36_offline.sh"
mkdir -p /tmp/oneapi-installer && cd /tmp/oneapi-installer
curl -L -o oneapi-2025.3.1.sh "$URL"
sudo sh ./oneapi-2025.3.1.sh \
    --silent --eula accept --components all \
    --install-dir /opt/intel/oneapi-2025.3
```

This **does not replace** an existing install at `/opt/intel/oneapi/` —
useful when keeping multiple versions side-by-side for A/B compiler
benchmarks.

To activate the new install for a shell:

```bash
source /opt/intel/oneapi-2025.3/setvars.sh
icpx --version  # → Intel(R) oneAPI DPC++/C++ Compiler 2025.3.1
```

[intel-baset-toolkit]: https://www.intel.com/content/www/us/en/developer/tools/oneapi/base-toolkit-download.html

### 2. Arch / CachyOS official package (single global install)

```bash
sudo pacman -S intel-oneapi-basekit
```

Tracks `cachyos-extra-znver4` — currently lags Intel by 1–2 releases.
At the time of this writing the repo ships **2025.0.4**; AUR tracks
ahead via `intel-oneapi-basekit-2025`.

### 3. AUR (single global install, newer than Arch repos)

```bash
paru -S intel-oneapi-basekit-2025   # or: yay -S
```

Conflicts with the Arch repo `intel-oneapi-basekit` package. Installs
to `/opt/intel/oneapi/` (not version-suffixed) so it replaces any
existing install.

## Multi-version coexistence

A common situation: the Arch repo / AUR install at `/opt/intel/oneapi/`
is held back at an older release (e.g. 2025.0.4) whose device images no
longer match the system's `level-zero-loader`, while a side-by-side
2025.3 install at `/opt/intel/oneapi-2025.3/` ships matching binaries.
`vmaf_bench --device sycl` against the older install silently
host-falls-back or fails `ze_init` — the symptom is "ran on CPU even
though a GPU is plugged in".

The bench / lint cycle has to point at the install whose runtime
matches the loader. The fork ships a helper that resolves the
appropriate `setvars.sh` and emits an `eval`-able env block:

```bash
# Activate the 2025.3 side-by-side install for this shell:
eval "$(scripts/ci/sycl-bench-env.sh 2025.3)"
icpx --version
# → Intel(R) oneAPI DPC++/C++ Compiler 2025.3.x

# Run the bench against the right runtime:
./build-sycl/tools/vmaf_bench --device sycl ...
```

The helper looks (in order) for:

1. `$ONEAPI_PREFIX` (explicit override).
2. `/opt/intel/oneapi-<version>/` (canonical side-by-side layout).
3. `/opt/intel/oneapi/<version>/` (Intel modulefile-style layout).
4. `/opt/intel/oneapi/` (fallback — emits a warning if the requested
   version doesn't match the install actually present).

If none resolves, the helper exits 1 with a pointer back to this
document. The helper only forwards `CMPLR_ROOT`, `LD_LIBRARY_PATH`,
`LIBRARY_PATH`, and `PATH` — the four variables `vmaf_bench`,
`icpx`, and `clang-tidy` actually consume — to keep the parent shell's
environment from accreting the ~40 oneAPI vars `setvars.sh` exports.

For the `clang-tidy` lane there's a parallel wrapper —
`scripts/ci/clang-tidy-sycl.sh` — that injects the SYCL include path +
`__SYCL_DEVICE_ONLY__=0` so SYCL TUs lint cleanly. See
[ADR-0217](../adr/0217-sycl-toolchain-cleanup.md).

## Verify the SYCL build picks up the new compiler

After installing a new oneAPI version:

```bash
# Activate the version you want for this shell:
source /opt/intel/oneapi-2025.3/setvars.sh        # side-by-side install
# OR
source /opt/intel/oneapi/setvars.sh               # default install

# Force a clean SYCL build (icpx version is baked into compile_commands):
rm -rf core/build-sycl-lint
meson setup core/build-sycl-lint libvmaf \
    -Denable_sycl=true -Denable_cuda=false
ninja -C core/build-sycl-lint
```

Verify the SYCL kernels still link:

```bash
ls -la core/build-sycl-lint/src/libvmaf.so.3.0.0
```

## Post-bump audit checklist

After a major-version oneAPI bump (e.g. 2025.0 → 2025.3), walk through
these items before declaring the bump complete. None block; each is a
follow-up backlog candidate.

- [ ] `atomic_ref` performance check — run `vmaf_bench` SYCL paths
  (`motion_sycl`, `adm_sycl`) on the canonical Arc / Battlemage host.
  Compare per-frame timings against the previous version's numbers
  (`testdata/sycl_bench_*.json`).
- [ ] `sub_group::shuffle_*` codegen — sample the IR for our VIF
  reduction loop (`integer_vif_sycl.cpp` line ~1100) and check whether
  the new compiler removes the `_mm`-style fallback we wrote against
  older Arc gen.
- [ ] `[[intel::reqd_sub_group_size(N)]]` — verify the compiler still
  honours our 32-lane requirements; some 2025.x releases added
  validation that fails compilation if the hardware can't support
  the requested SG size.
- [ ] `group_load` / `group_store` (2025.2+) — sketch a rewrite of
  the ADM DWT vert/hori passes on top of
  `sycl::ext::oneapi::experimental::group_load`. Profile the SLM tile
  load against the manual implementation — should reduce register
  pressure and may help on Battlemage.
  [Research-0086 §A.4](../research/0086-sycl-toolchain-audit-2026-05-08.md)
  emitted GO; on implementation review the rewrite was
  **deferred** under
  [ADR-0332](../adr/0332-sycl-adm-dwt-group-load-deferral.md) —
  the `WG_SIZE × ElementsPerWorkItem` divisibility constraint and
  the multi-row source contiguity gap defeat the sketch. Re-opens
  when (a) a tile-geometry redesign yields integer divisibility and
  (b) Xe2 / Battlemage hardware is available to confirm the
  register-pressure delta.
- [x] OpenVINO EP version bump — newer ORT bundled with the basekit
  exposes the NPU plugin via `device_type=NPU` on the existing
  `OpenVINOExecutionProvider`. **Done 2026-05-08** in
  [ADR-0332](../adr/0405-openvino-npu-ep-wiring.md): adds
  `--tiny-device=openvino-npu` (plus `openvino-cpu` / `openvino-gpu`
  for explicit OpenVINO device-type pinning). End-to-end NPU
  silicon validation still pending a contributor with Meteor /
  Lunar / Arrow Lake hardware.
- [ ] C++23 surface — icpx 2025.3 is LLVM-20-based; C++23 features
  (`std::expected`, `std::print`, `if consteval`) are usable but not
  yet adopted in any fork-local TU. Defer until a clear use case
  (likely the tiny-AI dispatch layer when the NPU EP lands).

## Verify SYCL clang-tidy still works

The fork's icpx-aware wrapper (T7-13(b), [ADR-0217](../adr/0217-sycl-toolchain-cleanup.md))
injects the oneAPI SYCL include path so stock LLVM `clang-tidy` resolves
`<sycl/sycl.hpp>`:

```bash
echo "core/src/sycl/picture_sycl.cpp
core/src/feature/sycl/integer_adm_sycl.cpp
core/src/feature/sycl/integer_motion_sycl.cpp
core/src/feature/sycl/integer_vif_sycl.cpp" \
  | parallel -j$(nproc) "scripts/ci/clang-tidy-sycl.sh \
      -p core/build-sycl-lint --quiet {}" \
  | grep -E "warning:|error:" \
  | wc -l
# Expected: 0 across all four files. T7-7 cleared the SYCL findings;
# T7-13(b) closes the residual `'sycl/sycl.hpp' file not found`
# clang-diagnostic-errors that previously left these files excluded
# from the changed-file CI lint gate.
```

If the wrapper can't locate `<sycl/sycl.hpp>` automatically, point it
at the install:

```bash
ICPX_ROOT=/opt/intel/oneapi-2025.3/compiler/latest/linux \
  scripts/ci/clang-tidy-sycl.sh -p core/build-sycl-lint --quiet <file>
```

## CI vs local

CI uses the `intel/oneapi-runtime-toolkit` action which installs
whatever version Intel currently publishes as the "stable" tag
(updated independently of this fork). The CI lane therefore may pick
up a newer version than the local pin documented here. Local-vs-CI
divergence on the SYCL kernel binaries is acceptable as long as both
build cleanly and the `Build — Ubuntu SYCL` matrix row stays green;
bit-identical SYCL output is **not** a guaranteed invariant across
Intel oneAPI releases.

## Related

- [ADR-0217](../adr/0217-sycl-toolchain-cleanup.md) — SYCL toolchain
  consolidation; `icpx` is the chosen DPC++ implementation.
- [`docs/backends/sycl/overview.md`](../backends/sycl/overview.md) —
  user-facing SYCL backend reference (`--sycl_device` etc.).
- [`docs/backends/sycl/bundling.md`](../backends/sycl/bundling.md) —
  shipping a self-contained binary without an end-user oneAPI install.
- [`build-flags.md`](build-flags.md) — `enable_sycl` meson option.
