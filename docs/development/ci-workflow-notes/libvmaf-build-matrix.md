# Design notes: libvmaf-build-matrix.yml

These notes were moved out of `.github/workflows/libvmaf-build-matrix.yml` so
the workflow stays under the 64 KiB input bound of the praetor engine. They are
the workflow's original comments, grouped by job, unchanged.

## general

```text
Builds — Linux/macOS/Windows/ARM x CPU/SYCL/CUDA.
The display name is deliberately short: GitHub's badge.svg paints it
into the README status badge. See ADR-1227.
ADR-0331 — `ready_for_review` lets the per-job draft gate fire
CI when a draft PR is promoted to ready-for-review.
No trigger-level paths-ignore: every required job of this workflow
must report on every pull request (ADR-3293), and each job selects
its own work through the CI tier and the impact planner (ADR-2169,
ADR-1140). ADR-0341's docs-only paths-ignore no longer exists.
Ported from the now-deleted windows.yml (ADR-0115). Cancels superseded
runs on the same ref so a force-push doesn't queue duplicate matrix runs.
```

## tier

```text
ADR-3279: plans whether a light-tier pull request owes the Windows MSVC+SYCL
leg of `windows-gpu-build` (selector `windows_msvc_sycl` of
.github/ci-impact.json: C, C++ and Meson files under core/, and the leg's
own inputs).
```

## impact

```text
===== Linux / macOS / ARM builds =====
```

## libvmaf-build

```text
--- Vulkan backend removed (ADR-0726) ---
The Vulkan backend was removed in ADR-0726; the Ubuntu Vulkan
(T5-1b runtime) and macOS MoltenVK (advisory) matrix rows that
passed -Denable_vulkan=enabled have been dropped.  meson.build
no longer exports that option and configure would fail with
"Unknown option: enable_vulkan".
ADR-0726 removed the Vulkan/MoltenVK rows; ADR-1297 removed the last two
`experimental: true` rows (macOS clang, macOS clang+DNN) and with them the
`continue-on-error: ${{ matrix.experimental == true }}` expression that
guarded them. No row in this matrix is advisory: every one of them is a
required context, so a red leg blocks the merge rather than annotating it.
ADR-2169 — the tier owes this leg or not. A leg of the full tier on a
light-tier pull request does no work (it runs on the master push and on
a `ci: full` pull request); the leg stays a job because a matrix cannot
be filtered per leg at job level, but it costs one short step.
A leg the tier does not owe takes a shallow checkout and stops there.
A leg that runs needs the tags: `git describe` names the version
`vmaf -v` prints (core/include/meson.build). Not `run && 0 || 1`:
0 is falsy in an expression, so that form always gave 1.
---------- ccache persistence (Research-0089 §3.1) ----------
Linux + macOS build legs all wrap their host compiler in `ccache`
(matrix.CC = "ccache gcc" / "ccache clang"). Without a persisted
`~/.ccache` directory every PR cold-rebuilds; per Research-0089
the typical hit rate after warm-up is 60–85% and the wall-clock
saving is ~3–5 min/cell. Only the SYCL legs run a bare icx/icpx
without the ccache wrapper, so skip those; matrix.CC starts with
"ccache" on every other lane.

Cache key + path shape follows the official GitHub Actions
`actions/cache` cross-platform recipe — `~/.ccache` is ccache's
default cache dir on Linux + macOS, and per-OS / per-compiler
keying with restore-key fall-through is the documented pattern
for build caches that vary by toolchain. See
https://github.com/actions/cache (accessed 2026-05-09) and
https://github.com/actions/cache/blob/main/examples.md (same date)
— "C++ - ccache" example.
v2 prefix: forces fresh entries after actions/cache v4→v5
upgrade (old v4-format entries cause deserialization failures
on macOS Vulkan lane — ADR-0635).
gcc-14 is preinstalled on ubuntu-24.04; clang tops out at 18.1.3
there, which accepts -std=c++26 but ships a libstdc++ with no
std::expected, so core/src/dict.cpp cannot build (ADR-0692 moved
the tree to C23 + C++26). Pull clang from apt.llvm.org rather than
degrading the language standard to suit the distro's compiler.
libomp-22-dev, not the unversioned libomp-dev: the
latter matches the *system* clang (18). The tox leg
builds libsvm-official from source with $CXX, and its
svm.cpp needs <omp.h> from the same LLVM major, else
"fatal error: 'omp.h' file not found".
---------- ONNX Runtime (DNN legs only) ----------
Mirrors the install pattern from the dedicated Tiny AI job in
tests-and-quality-gates.yml so DNN-on builds across compilers /
OSes share the same pinned ORT version. See ADR-0120.
Homebrew ships onnxruntime as a keg-only formula with its own
libonnxruntime.pc. Make pkg-config see it. Note: neither the
brew keg nor Microsoft's official `onnxruntime-osx-arm64-*.tgz`
ship `libonnxruntime_providers_shared.dylib` (they statically
link providers into the main lib). The DNN test
`test_ep_fp16::test_auto_falls_through_to_cpu` triggers a
provider-bridge dlopen of that dylib and fails on macOS as a
result; the test is skipped at compile time on `__APPLE__`
to preserve the rest of the DNN suite for macOS coverage.
---------- Intel oneAPI (SYCL) ----------
Pinned to oneAPI 2025.3 (icpx 2025.3.x) and Level Zero v1.28.0.
2025.0 was an ABI-breaking release, so pin to the 2025.3 minor meta
package rather than the unversioned 'latest' to avoid silent bumps.
Override the matrix-level CC=icx / CXX=icpx for *this* step only:
the Level Zero loader is plain C and we don't have icx on PATH yet
at this point in the same step (apt installs it below, then the
cmake invocation runs before the next step picks up GITHUB_PATH).
Force gcc here; the meson setup step further down picks up icpx
via the matrix env once setvars.sh adds /opt/intel/oneapi/compiler
to PATH.
Lifts the anonymous GitHub API limit for the NEO release lookup.
ocloc compiles the SYCL ahead-of-time images; the Linux compiler
does not ship it and meson setup refuses without it (ADR-1360).
Level Zero headers + loader, pinned by LEVEL_ZERO_VERSION in
build-config.env. Three different versions used to be in the tree at
once (v1.28.0 here, 1.32.0 in dev/Containerfile, the distro package
elsewhere); a skew here surfaces as "no device available" rather
than as a build error (ADR-1231).
---------- CUDA toolkit ----------
Pinned to CUDA_VERSION in build-config.env (scripts/ci/install-cuda-toolkit.sh
reads it), the same release as build.yml's Linux and Windows legs and the
dev container. ADR-0603: Ubuntu 26.04 (glibc 2.43) needs CUDA >=
13.2 to avoid the rsqrt(double) noexcept C2059/C2085 NVCC cascade.

Installed from NVIDIA's apt repository rather than through
Jimver/cuda-toolkit. That action carries its own table of downloadable
releases, so the fork could only pin what the action already knew, and
its newest release (v0.2.36, 2026-08-02) tops out at 13.3.1 with no
13.4.x entry at all — the CUDA pin was gated on a third party cutting
a release. T-CI-JIMVER-CUDA-133-NOT-AVAILABLE was the same coupling
one version earlier. NVIDIA's repository has no such lag and is
already where dev/Containerfile installs from.

13.x drops sm_50/60/70 (Maxwell/Pascal/Volta). ADR-1223 additionally
drops sm_75 (Turing): the gencode floor is compute_80 (Ampere).
Minimum driver for 13.x is >= 580.
---------- ROCm / HIP toolchain (T7-10b) ----------
ROCm 10 has no apt channel. Since ROCm 7.14 AMD builds and
releases through "TheRock"; repo.radeon.com/rocm/apt/ tops out at
7.2.4 (its own `latest` resolves there, and apt/7.14, apt/10.0.0
and apt/10.1.0 all 404), the manylinux channel stops at
rocm-rel-7.2.4, and the TheRock wheel index carries only 7.14.0
alphas. The official container image is the only stable,
digest-pinnable ROCm 10 artifact -- see ADR-1225.

`docker pull` is not viable here: the image is 8.9 GB compressed /
31 GB extracted and this leg already carries the CUDA toolkit and
oneAPI on the same runner. `install-rocm-from-image.sh` instead
streams each layer blob from the registry straight into tar,
extracting only /opt/rocm and skipping the math libraries libvmaf
never links, so peak disk is the ~5.5 GB result rather than the
29 GB image.

Cost: ~8 GB download + ~3-5 min wall-clock per HIP-lane run,
comparable to the apt install it replaces.
The ROCm image is defined once in build-config.env (ADR-1231),
never pinned again here. It was previously a digest literal in
this file and a second one in libvmaf-build-matrix.yml.
Surface the ROCm tooling on PATH / pkg-config / ld so meson's
`dependency('hip-lang')` and the link of `amdhip64` resolve
without per-step exports.
---------- Configure ccache cap + stats reset ----------
ccache dir is restored above by `actions/cache`; size-cap it so
stale objects evict in LRU order rather than the cache growing
without bound across runs (the hosted runner has a hard 10 GB
cache budget per repo; we want headroom for fixture caches too).
`ccache -z` zeroes the stats so the post-build `ccache -s` shows
only this run's hit/miss counts. See
https://ccache.dev/manual/latest.html#_configuration (accessed
2026-05-09) for `max_size` semantics.
---------- Build ----------
SYCL build needs setvars.sh on PATH/LD_LIBRARY_PATH so meson
can probe libsycl + level-zero headers via icpx -fsycl. CPU
/ CUDA matrix entries skip this — they don't have setvars.sh.
Surface ccache hit/miss counters in the CI log so the savings
claim from Research-0089 §3.1 can be verified per-run. Runs
whether the build succeeds or fails (always()) so a failed
build still reports whether ccache was warming up.
Verify pkgconfig for static linking
ADR-2828: both static rows build with this compiler. Naming it here
lets the build-warnings gate read the smoke compile; the first
command fails when a row's compiler stops agreeing.
Static libs must include -lpthread and -lm
... and the C++ runtime, because libvmaf contains C++ translation
units (feature_extractor.cpp, luminance_tools.cpp, vendored libsvm,
...). Netflix/vmaf#1178.
Grepping the flag list is not the test that actually reproduces the
downstream FFmpeg failure — LINKING is. Build a C consumer with the
C driver, using exactly what pkg-config reports.
Use the leg's OWN compiler, not bare `cc`. The matrix builds with
`ccache gcc-14` / `ccache clang-22`, while `cc` is the image
default (a different gcc), so a bare `cc` link tests a toolchain
the archive was not produced with. $CC is intentionally unquoted:
its value is two words ("ccache gcc-14") and must word-split.
(b_lto is meson-default false here, and explicitly false on the
SYCL/CUDA legs, so the archive holds plain objects rather than LTO
IR — a plugin mismatch is not in play, but matching the compiler
still costs nothing.)
pkg-config prints space-separated flags; read them into arrays
rather than relying on unquoted word splitting.
DNN legs: run the dedicated dnn suite first so its logs are easy
to find if the build regresses ORT compatibility. The full test
run below also exercises dnn via the repository test runner; this step exists
for log isolation only.
`sudo $(which meson)` resolves the meson absolute path before
sudo strips PATH — pip-installed meson lives under the runner
user's $HOME so a bare `sudo meson` reports "command not found".
Sudo is needed because the preceding `sudo ninja install` step
left build artifacts owned by root.
The suite's Go-running tests find their modules in the cache: the
download happens here, retried, and fails here when it cannot succeed
(scripts/ci/prefetch_go_modules.py, as the user the suite runs as).
CPU-only: full test suite + tox.
---------- SSH debug session (macOS × workflow_dispatch only) ----------
Fires ONLY when:
  (a) the job is running on macOS (any macOS matrix leg), AND
  (b) a preceding step has failed (failure()), AND
  (c) the workflow was triggered by a manual workflow_dispatch event.

The workflow_dispatch gate is critical: it prevents the session from
opening on every failing PR push, which would strand a runner for up
to 30 minutes per failure. Manual dispatch is deliberate — the operator
signals intent to debug interactively.

Action pinned to commit SHA (not just tag) per the fork's
`helpers:pinGitHubActionDigests` Renovate policy.
SHA corresponds to mxschmitt/action-tmate v3.24.
See docs/development/ci-tmate-debug.md for usage.
Wait up to 30 min for the operator to connect; if nobody connects
within that window the step unblocks and the job completes.
(ADR-0726: Vulkan removed — MoltenVK install + smoke test steps deleted)
Restore-only; the save is split out below and gated on success().
The combined actions/cache action saves from a post-job step that runs even
when the job was CANCELLED or FAILED, and the key is content-hashed over
python/test/*_test.py, so any PR touching a Python test gets its own key. A
run cancelled part-way through then published a partially-populated
python/test/resource tree under that fresh key, and every later run on the
branch hit it EXACTLY. download_reactively (compat/python-vmaf/config.py)
re-fetches a fixture only when the file is ABSENT, so a short-but-present
file is never repaired. See the same split in build.yml.
Insulates the tox lanes from the recurring HTTP-502s on
raw.githubusercontent.com when fetching python/test/resource/
fixtures via download_reactively (compat/python-vmaf/config.py).
A restored fixture that is empty, or that holds an HTML error page or
a Git-LFS pointer instead of the payload, is worse than a missing one:
download_reactively re-fetches only ABSENT files, so the bad copy is
never repaired and the test fails with "no frames decoded". The
success()-gated save below stops new poisoning; this heals caches
already poisoned, which otherwise need a manual cache deletion.
--restore-tracked: the cache also carries the files git tracks under
python/test/resource in the revision of the run that saved it; a
restore-keys hit writes them over this checkout, so they are put
back to the commit's (T-CI-FIXTURE-CACHE-STALE-TRACKED-2026-10-06).
success() only: a cancelled or failing run must never publish a
partial fixture tree under this key. Placed after both tox legs.
ADR-1495: an icx / icpx build links glibc's libm, not Intel's libimf,
so its CPU scores equal a GCC build's. The SYCL legs are the icx
builds of this matrix; the test reads their libvmaf.so and vmaf.
Sudo because the install step left the build tree owned by root.
GPU builds: run tests that don't need hardware
HIP runtime build: runs the HIP smoke test (no GPU needed —
the smoke harness short-circuits on `vmaf_hip_device_count() == 0`
per ADR-0212). Skips kernel-launch tests entirely; the GHA
runner has no AMD GPU.
---------- Artifacts ----------
===== Windows (MSYS2 / MinGW-w64 UCRT64) =====
Folded in from the now-deleted .github/workflows/windows.yml so the
full build matrix lives in one workflow file. See ADR-0115.
MINGW32 is intentionally disabled (vmaf score mismatch — upstream note);
32-bit support would need a SIMD audit before re-enabling.

Job key renamed `build` → `windows` and display name set to Title Case
per ADR-0116. Migrated from deprecated MINGW64 to UCRT64 (ADR-1387).
```

## windows

```text
ADR-1254. The MS x64 ABI guarantees only 16-byte stack alignment and
its unwind contract prevents the frame realignment gcc emits on SysV,
but gcc's MinGW target still allocates 64-byte-aligned zmm spill slots
addressed off %rsp — a #GP on most call paths, surfacing as an access
violation on 0xFFFFFFFFFFFFFFFF. The test step below cannot catch it
because the runner's CPU has no AVX-512, so the AVX-512 kernels never
execute here. This reads the emitted code instead of running it.
The scan takes about 6 minutes. A limit of its own stops a stalled
scan here, with this step named, instead of letting it eat the job's
45 minutes (master 190eea419, 2026-10-20: 39 minutes in this step).
The script prints its wall time and slowest files.
The suite's Go-running tests find their modules in the cache: the
download happens here, retried, and fails here when it cannot succeed
(scripts/ci/prefetch_go_modules.py, as the user the suite runs as).
--print-errorlogs surfaces the failing test's stdout/stderr
inline in the CI log instead of only writing it to
meson-logs/testlog.txt, so cross-platform debugging (e.g.
MinGW64-specific locale assertions) does not require an
artifact round-trip.
===== Windows GPU build-only (MSVC + CUDA, MSVC + oneAPI SYCL) =====
Build-only legs — no GPU on the windows-2025 runner, so the test
step is intentionally omitted. The point is verifying that the
CUDA / SYCL backend code compiles & links on the MSVC toolchain,
closing the platform-coverage hole left by the Linux-only GPU
matrix entries above. See ADR-0121.

MSVC env is set up via TheMrMilchmann/setup-msvc-dev@v4 (Node 24
replacement for ilammy/msvc-dev-cmd@v1.13.0, which was deprecated
on Node.js 20 — see ADR-0635). Functionally identical: runs
vcvarsall.bat and exports PATH/INCLUDE/LIB for cl.exe.
CUDA toolkit is installed directly from NVIDIA's Windows network
installer, pinned to the same release as the Linux CUDA legs
(CUDA_VERSION in build-config.env). Both sides now install from
NVIDIA directly: the Windows leg never used Jimver/cuda-toolkit
because v0.2.35 could exit before setup on current windows-2025
images (ADR-0664), and the Linux legs left it behind because its
own release table capped which CUDA versions could be pinned.
oneAPI is installed via the official `oneapi-src/oneapi-ci`
offline-installer pattern (`curl` the Base Toolkit installer +
extract + run bootstrapper). We tried `rscohn2/setup-oneapi@v0`
first (Linux-only — all install URLs `.sh`) and Chocolatey's
`intel-oneapi-basekit` (not on the community feed); neither
works on `windows-2025`. Pinned to oneAPI 2025.3.0.372 to
lag the Linux SYCL leg, which now follows ONEAPI_APT_PACKAGE.
The CUDA leg additionally injects `/experimental:c11atomics` into
`CFLAGS` / `CXXFLAGS` because libvmaf uses C11 atomics that
MSVC's `<stdatomic.h>` refuses without that flag; icx-cl (SYCL leg)
uses its own `<stdatomic.h>` and does not take it.

The two job names are pinned to required status checks on master
immediately after merge (see ADR-0121 §Consequences).
```

## windows-gpu-build

```text
ADR-3279: the full tier runs both rows. On the light tier the job starts when
the planner selects the SYCL row, and also when the planner itself failed
(fail closed); the CUDA row then stops at its first step.
---------- MSVC dev environment ----------
Exports cl.exe + the rest of the MSVC toolchain into PATH /
INCLUDE / LIB. Required by both backends (CUDA's nvcc shells
out to cl.exe; oneAPI's icx-cl wraps cl.exe).
Switched from ilammy/msvc-dev-cmd@v1.13.0 (Node.js 20,
deprecated 2026-06-02) to TheMrMilchmann/setup-msvc-dev@v4
(Node.js 24) per ADR-0635. Same vcvarsall.bat semantics;
no inputs required — defaults to amd64.
---------- CUDA toolkit (CUDA leg only) ----------
Same pin as the Linux CUDA legs (CUDA_VERSION in build-config.env, read
by install-cuda-toolkit.ps1). 13.x itself drops
Maxwell/Pascal/Volta; ADR-1223 additionally drops Turing (sm_75), so
meson.build's gencode floor is compute_80 and the `< 13` sm_50 guard
is gone. Bumped 13.0.0 -> 13.2.0 per ADR-0603, 13.2.0 -> 13.3.1 per
ADR-1223; build-config.env has moved on since (Renovate-driven).
---------- nv-codec-headers (CUDA leg only) ----------
libvmaf's CUDA path includes `<ffnvcodec/dynlink_cuda.h>`
(see core/src/meson.build §324). Both legs fetch the commit
build-config.env pins with scripts/ci/fetch-nv-codec-headers.sh.
The Linux leg runs `make install`; on Windows the package is
header-only, so copy `include/ffnvcodec/` into a known prefix
and append that prefix to `INCLUDE` so `cl.exe` picks it up.
---------- Strip ccache from PATH (Windows) ----------
`windows-2025` ships `C:\Strawberry\c\bin\ccache.exe` on
PATH. Meson's compiler-detection wraps nvcc with it
unconditionally → "Compiler ... ccache nvcc cannot compile
programs" (the wrap happens at meson configure time;
`CCACHE_DISABLE=1` only affects ccache at runtime, not
meson's wrap decision). Rename the binary so meson can't
find it.
ADR-2170: the configure steps run under cmd, so the one helper that turns warnings
into errors runs here and hands its arguments over. A row without `werror` gets none.
---------- Build ----------
CFLAGS/CXXFLAGS carry `/experimental:c11atomics` — without
that flag MSVC's `<stdatomic.h>` errors with "C atomic
support is not enabled" (libvmaf uses C11 atomics; meson's
compiler check rejects the toolchain otherwise).
---------- Level Zero loader (SYCL leg only) ----------
core/src/meson.build:492 calls
`cc.find_library('ze_loader', required : true)` and
`core/src/sycl/common.cpp:26` includes
`<level_zero/ze_api.h>`. The Linux SYCL leg picks both up
from `intel-level-zero-gpu` / `level-zero-dev` apt packages.
Windows oneAPI BaseKit does NOT ship `ze_loader.lib`, so
build the loader from source (cmake build, header-only +
one .lib output). Mirror the `oneapi-src/level-zero` README
quick-build recipe; pin to v1.18.5 to match the version the
Linux Ubuntu 26.04 runners install via apt.
Pin `c++latest` because the fork's C++ sources require C++23
`std::expected`, while icx-cl under MSVC ABI mode advertises the
Microsoft-style c++14/c++17/c++20/c++latest option set. No
`/experimental:c11atomics` here: icx-cl compiles C11 atomics with its
own <stdatomic.h> and reported the option as unused on every compile.
Q-299 (Netflix/vmaf 3b4dd350e): a static MSVC build installs vmaf.lib
and vmafx.lib, the files link.exe opens for -lvmaf / -lvmafx, and the
pkg-config files that name them.
windows-2025 has no GPU, so no kernel runs here. Whether the linked
programs registered their SYCL device images needs none:
test_sycl_kernel_registration asks the SYCL runtime for its kernel
IDs. Until the MSVC device link (ADR-1364) that registry was empty,
and every SYCL kernel submit failed on real Arc / UHD hardware while
this build-only leg stayed green. ze_loader.dll comes from the
Level Zero prefix built above.
```

## windows-arm64

```text
Full history: test_vmafx_api_abi_append_only and test_crd_compat of
the fast suite compare with the merge base of HEAD and origin/master
and skip (77) without it (scripts/ci/tests/test_meson_suite_checkout_history.py).
actions/python-versions publishes win32/arm64 builds of 3.14.7
(versions-manifest.json, checked 2026-09-19): same pin as the
x64 Windows legs.
Same reason as the x64 MSVC legs: a ccache.exe on PATH makes
meson wrap the compiler at configure time.
Fail before the build if vcvarsall handed us anything but the
ARM64 compiler: cl.exe's banner names its target architecture.
ADR-2170: this leg prints no warnings, so cl.exe and link.exe make
them errors. The configure step runs under cmd; the helper runs here.
`/experimental:c11atomics` as on every MSVC leg (see the
windows-gpu-build comment). `enable_asm` stays at its default,
which on aarch64 means the NEON translation units.
The PE header's Machine field must read 0xAA64 (ARM64). An x64
binary would still run here under emulation and pass the tests,
so this pins that the lane exercised native ARM64 code.
The suite's Go-running tests find their modules in the cache: the
download happens here, retried, and fails here when it cannot succeed
(scripts/ci/prefetch_go_modules.py, as the user the suite runs as).
The MinGW UCRT64 lane runs the whole suite; this lane starts with
`fast`. --print-errorlogs mirrors that lane so a failing test's
output lands in the CI log instead of only in testlog.txt.
```
