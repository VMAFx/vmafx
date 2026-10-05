<!-- markdownlint-disable MD013 -->
# Measuring the clang-tidy lanes

The whole-tree clang-tidy ratchet
([ADR-1142](../adr/1142-whole-codebase-standards.md),
[CI guide](ci.md#whole-tree-lint-ratchet-adr-1142)) compares every file with a
committed count in `scripts/ci/tidy-baseline-<lane>.json`. A count is only
meaningful on the toolchain it was measured with, so every lane is measured in
one place: the dev container
([ADR-1471](../adr/1471-tidy-lanes-in-dev-container.md)).

## Measure a lane

```bash
make tidy-lane LANE=cpu          # compare with the committed baseline
make tidy-lane LANE=all          # cpu, clang, cuda, hip, sycl and arm64, one after the other
make tidy-lane-write LANE=hip    # rewrite scripts/ci/tidy-baseline-hip.json
```

or call the script the targets wrap:

```bash
scripts/dev/tidy-lane.sh cpu cuda
scripts/dev/tidy-lane.sh --write all
scripts/dev/tidy-lane.sh --write --only core/src/feature/hip/ciede_hip.c hip
scripts/dev/tidy-lane.sh --help
```

You need Docker and the dev image `vmaf-dev-mcp:local`
([dev container guide](dev-mcp.md)); another tag goes in `--image` or
`VMAFX_DEV_IMAGE`. The first step of a run downloads clang-tidy 22 from
apt.llvm.org and meson from PyPI (hash-locked, `requirements/locks/build.txt`),
so the run needs network access; the `arm64` lane also installs the aarch64
cross compiler and `qemu-user` from the Ubuntu archive. A lane takes about
five minutes on eight cores. `--jobs N` (default 8) sets the cores for the
build and for clang-tidy and caps the container at the same number.

The exit code is the ratchet's:

| Code | Meaning |
| --- | --- |
| `0` | The baseline matches. |
| `2` | A file is above its baseline. |
| `3` | A file is below its baseline. |
| `4` | The lane did not build, or a translation unit did not parse. |
| `5` | A usage error, or a clang-tidy version the baseline was not measured with ([below](#the-clang-tidy-version)). |

With several lanes the highest code wins. Reports (`tidy-ratchet-<lane>.json`,
with every diagnostic behind the counts) and logs (`<lane>.log`) land in
`~/.cache/vmafx-tidy-lanes/`, or in the directory given with `--out`.

## After a change to C, C++, CUDA, HIP or SYCL sources

1. Fix the findings in the files you touched; a touched file ends at zero
   ([agent hard rules](agent-hard-rules.md), rule 10).
2. Run the lanes that read the file. A file under `core/src/feature/cuda/`
   is read by `cuda`; a file every build compiles (`core/src/libvmaf.c`) is
   read by all of them. `make tidy-lane LANE=all` is the safe choice.
3. Exit `3` means the file is cleaner than its baseline: run
   `make tidy-lane-write LANE=<lane>` and commit the JSON with your change.
   To tighten only your files and leave the rest of the baseline untouched
   ([ADR-1243](../adr/1243-tidy-scoped-baseline-tightening.md)), pass
   `--only <path>` once per translation unit.
4. Exit `2` means a file got worse. Fix the code. A baseline is never raised
   by hand.

`--write` copies the rewritten baseline back into your checkout. Nothing else
in the checkout is written, and nothing is mounted into the container.

### The clang-tidy version

A baseline records the clang-tidy it was measured with (`clang_tidy_version`,
22.1.8 today), and the counts of two versions are not comparable. apt.llvm.org
serves only the newest build of the 22 series, the same one the hosted job
gets.

When that build moves on, a check and a scoped write stop with exit `5` and
name both versions; nothing is measured.

Moving the baselines to the new version is one deliberate step, reviewed like
any other change of the counts:

```bash
make tidy-lane-write LANE=all
```

The hosted job then measures against the same version. A workstation's own
clang-tidy plays no part: the container never uses the host's tools.

## Why not on the host

The numbers depend on the C library, the compilers and the device toolchains,
not only on clang-tidy's version:

- **C library.** glibc 2.44 expands `assert(e)` in C++ so that clang-tidy 22
  reports `misc-static-assert` / `cert-dcl03-c` for every runtime assertion
  (`T-TIDY-GLIBC-244-STATIC-ASSERT-FALSE-POSITIVE-2026-10-02` in
  [`docs/state.md`](../state.md)); glibc 2.43 in the container and on the
  hosted runners does not. A baseline written on such a host carries those
  findings, and the hosted job then fails every file as "below its baseline".
  That is what the first complete hosted run after two days of local landing
  showed: 24 files below a baseline that named gcc 16 as its compiler.
- **Device compilers.** A host without `hipcc` configures the HIP backend
  with `-Denable_hipcc=false`, where 21 host files compile to `-ENOSYS` stubs.
  The lane then lints the stubs, not the code a device runs.
- **Optional libraries.** With ONNX Runtime installed, the DNN sources compile
  their real bodies and two more translation units exist. The hosted runner
  has none.
- **clang-tidy itself.** A workstation's package manager moves it: on
  2026-10-02 the workstation's clang-tidy went from 22.1.8 to 23.1.1, which
  reports more and which the scoped write refuses against a 22.1.8 baseline.

The container pins all of that: Ubuntu 26.04 (the hosted runners' release),
gcc-15, CUDA with `nvcc`, ROCm with `hipcc`, oneAPI with `icpx`, ONNX Runtime,
and clang-tidy 22 from the hosted job's source.

## What the script does

`scripts/dev/tidy-lane.sh`:

1. creates a throwaway container of the dev image, capped to `--jobs` CPUs;
2. copies the checkout's tracked files and the files not yet added (not the
   ignored ones) into it as a tar stream, so uncommitted edits are measured
   and a host build directory is not;
3. installs clang-tidy 22 from apt.llvm.org's `llvm-toolchain-<codename>-22`,
   the package the hosted job installs with `llvm.sh 22`, unless the image
   already has it. The archive key is checked against a pinned SHA-256;
4. runs `make tidy-ratchet-build LANE=<lane>`: `meson setup` with the lane's
   configuration, then a full build, which leaves `compile_commands.json` and
   the generated headers;
5. runs `make tidy-ratchet` or `make tidy-ratchet-write` for the lane;
6. copies the report, the log and, with `--write`, the baseline out and
   removes the container.

## Lane configurations

One definition, in the `Makefile`: `TIDY_RATCHET_COMPILERS_<lane>` and
`TIDY_RATCHET_SETUP_<lane>`.

| Lane | Compilers | `meson setup` options | Parsed by |
| --- | --- | --- | --- |
| `cpu` | gcc-15 / g++-15 | `-Denable_cuda=false -Denable_sycl=false -Denable_dnn=disabled -Denable_mcp=true -Denable_mcp_sse=enabled -Denable_mcp_uds=true -Denable_mcp_stdio=true -Db_lto=false` | clang-tidy 22 |
| `clang` | clang-22 / clang++-22 | the `cpu` options plus `-Dfuzz=true` | clang-tidy 22, `--select core/test/fuzz/ --select core/src/read_json_model.c` |
| `cuda` | gcc-15 / g++-15, nvcc | `-Denable_cuda=true -Denable_nvcc=true -Denable_sycl=false -Denable_hip=false -Denable_dnn=enabled -Db_lto=false` | clang-tidy 22, `--cuda-host-only -nocudalib` |
| `hip` | gcc-15 / g++-15, hipcc | `-Denable_hip=true -Denable_hipcc=true -Denable_cuda=false -Denable_sycl=false -Denable_dnn=enabled -Db_lto=false` | clang-tidy 22 for the host files, ROCm's clang-tidy for the `.hip` kernels, on their host compilation (`--cuda-host-only`: LLVM 24 lists the device job first, and clang-tidy analyses the first) |
| `sycl` | icx / icpx | `-Denable_sycl=true -Dsycl_icpx_aot_targets= -Denable_cuda=false -Denable_hip=false -Denable_dnn=enabled -Db_lto=false` | clang-tidy 22 through `scripts/ci/clang-tidy-sycl.sh` |
| `arm64` | aarch64-linux-gnu-gcc / g++ 15 (cross) | `--cross-file build-aux/aarch64-linux-gnu.ini --cross-file build-aux/aarch64-linux-gnu-qemu-user.ini -Denable_cuda=false -Denable_sycl=false -Denable_dnn=disabled -Db_lto=false` | clang-tidy 22, `--target=aarch64-linux-gnu --sysroot=/usr/aarch64-linux-gnu` |

### Notes per lane

- `-Db_lto=false` everywhere: the project default renders as GCC's `-flto=4`
  ([ADR-1172](../adr/1172-bound-lto-link-parallelism.md)), which clang
  rejects, so every translation unit would fail to parse.
- `cpu` disables the DNN runtime because the hosted runner has no ONNX Runtime
  and the two must measure the same translation units. The GPU lanes enable
  it, so the DNN bodies are measured there.
- `sycl` compiles SPIR-V only. The ahead-of-time device list changes backend
  arguments that `scripts/ci/gen-sycl-compile-commands.py` removes from the
  lint database anyway.
- **Kernels.** meson compiles `.cu`, `.hip` and the SYCL sources through
  custom commands, which `compile_commands.json` does not list.
  `scripts/ci/gen-gpu-compile-commands.py` (cuda, hip) and
  `scripts/ci/gen-sycl-compile-commands.py` (sycl) add them; both stop the
  lane when a kernel rule exists that they could not read, so a lane never
  reports a clean measurement of a database without its kernels.
- **`.hip` kernels and ROCm's clang-tidy.** ROCm 10's device headers call
  `__builtin_amdgcn_is_invocable`, a builtin of the LLVM that ROCm ships.
  Stock clang-tidy 22 stops there ("builtin functions must be directly
  called"), so `scripts/ci/clang-tidy-hip.sh` sends `.hip` files to
  `/opt/rocm/llvm/bin/clang-tidy` (hipcc's own LLVM) and everything else to
  clang-tidy 22. The version a baseline records is clang-tidy 22's; the
  kernel tool's version follows the ROCm image pinned in `build-config.env`.
- **`arm64`** is the cross lane of
  [ADR-1283](../adr/1283-whole-tree-ratchet-arm64-lane.md): the NEON and SVE2
  sources that no x86 build compiles. It uses the in-tree cross file and a
  second one, `build-aux/aarch64-linux-gnu-qemu-user.ini`, that names
  Ubuntu's `qemu-aarch64` (the first names `qemu-aarch64-static`, which
  Ubuntu 26.04 does not package); meson runs its compiler check through it.

- `clang` exists because libFuzzer is a clang feature: `-Dfuzz=true` is a
  configure error under gcc, so the five harnesses of `core/test/fuzz/` (and
  `core/src/read_json_model.c`, which only they compile) are in no gcc
  database. The lane builds the `cpu` configuration with clang and measures
  those files only (`--select`, a path prefix of `scripts/ci/tidy-ratchet.py`),
  so a file the other lanes own is not counted twice. The container installs
  `clang-22` and `libclang-rt-22-dev` from the same apt.llvm.org archive as
  clang-tidy.
- `cpu` configures the embedded MCP server (`-Denable_mcp=true` and its three
  transports), so `core/src/mcp/` and `core/test/test_mcp_*.c` are read. The
  hosted `Tidy Ratchet` job repeats the same options.

## What every translation unit is read by

Every tracked `.c`, `.cc`, `.cpp`, `.cxx`, `.cu`, `.hip`, `.mm` and `.metal`
file is in the `measured_sources` of at least one baseline
(`scripts/ci/tidy-baseline-<lane>.json`) or in
the shared lint exception list (`.config/lint-exceptions.d/clang-tidy-coverage.toml`).
`python3 scripts/ci/check-tidy-coverage.py` fails otherwise; it is a pre-commit hook
(`check-tidy-coverage`) and runs in CI with the rest of the hooks.

A new translation unit therefore has to land in a lane: configure the option
that builds it in the lane that reads it, and write its allowance with
`scripts/dev/tidy-lane.sh --write --only <path> <lane>` (the scoped write also
records the file as measured). A file no lane can read gets one entry in the
exception list: the path, the rule `clang-tidy-coverage` (the file name), a reason that names the
missing tool or toolchain, and an expiry date. An entry fails the check once
it has expired, when its file is gone, and when a lane reads the file after
all; extend or delete it, never leave it.

| Group | Read by | Why the rest is excepted |
| --- | --- | --- |
| C, C++ and the device kernels | `cpu`, `clang`, `cuda`, `hip`, `sycl`, `arm64` (the six container lanes) | |
| Metal host code (`core/src/metal/*.mm`, `core/src/feature/metal/*_metal.mm`) and the Metal-only C tests | `metal` (macOS, below) | |
| Metal kernels (`core/src/feature/metal/*.metal`) | nothing | Upstream clang-tidy has no Metal language mode (`clang -x metal` answers "language not recognized"). |
| Pelorus mirror (`core/src/interop/pelorus_*.c`, `core/test/test_pelorus_interop.c`) | nothing here | Byte-identical mirror of VMAFx/pelorus (ADR-1113); `scripts/ci/pelorus-mirror-paths.txt` keeps it out of every lane. It is tidied in the pelorus repository's own CI at the pinned SHA, fixed there and re-vendored. One entry per file; the check fails on a mirror file without one. |
| `.config/hiss/testdata/` | nothing | The planted defect is the fixture. |
| `cmd/vmafx-node/bpf/` | nothing | Includes `vmlinux.h`, generated from a running kernel's BTF. |
| `core/tools/compat/win32/getopt.c`, `core/tools/test/test_vmaf_windows_utf8_argv.cpp` | nothing | Built on Windows only. |
| `core/src/feature/tad_rust.c`, `core/test/test_tad_rust.c` | nothing | Need `-Denable_rust_features=true` (cargo and cbindgen), absent from the image. |
| Pelorus interop mirror (`scripts/ci/pelorus-mirror-paths.txt`) | nothing | Byte-exact mirror (ADR-1113); fixes go upstream. |
| `scripts/dev/upstream_parity_harness.c`, `scripts/dev/hip_dispatch_drop_probe.hip` | nothing | Built by hand or by a script, outside meson. |

`core/src/feature/hip/integer_adm/adm_decouple_inline.hip` is a header that two
kernels include; the check does not count it as a unit.

## The macOS `metal` lane

The Objective-C++ sources of the Metal backend need Apple's SDK, which no
container has. The `Tidy Metal` workflow
([`tidy-metal.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/tidy-metal.yml))
runs on `macos-latest` weekly, on a pull request that touches the Metal host
sources, and on dispatch (a macOS runner bills ten times a Linux one, so it is
not a per-pull-request gate, as for the tester bundle): Homebrew's `llvm@22` clang-tidy (the Linux lanes'
major), Apple clang's compile commands, `-isysroot` named explicitly because
Homebrew's clang-tidy has no implicit SDK. It measures
`core/src/metal/`, `core/src/feature/metal/` and the four Metal-only tests
(`--select`) against `scripts/ci/tidy-baseline-metal.json`. The configuration
is `TIDY_RATCHET_COMPILERS_metal` / `TIDY_RATCHET_SETUP_metal` in the
`Makefile`, repeated by the job; `test_tidy_lane_container.py` compares them.
The job is not a required check yet. It becomes one after it has passed on
`master`, the path the SYCL lane took (ADR-1297). The artifact of a run
(`tidy-ratchet-metal`) holds every diagnostic behind the counts; the baseline
is the same JSON without them.

Today the baseline holds 939 findings across the 21 Objective-C++ files and
five C tests, not zero: the first hosted measurement was 1640, and the
fixes `clang-tidy` could make itself brought it down. Dispatch the workflow with
`fix=true` to have the runner apply those fixes and upload them as
`tidy-metal-fixes.patch`; apply the patch to the `.mm` files only (the fixes
to the headers break the Metal shader compiler and the C translation units that
include them), push, and dispatch again without `fix` to compile and measure.
The ratchet refuses growth; the remaining findings are tracked by
`T-TIDY-METAL-HOST-FINDINGS-2026-10-05` in [state](../state.md).

## What the hosted job covers

The required check `Tidy Ratchet` (`.github/workflows/lint-and-format.yml`)
measures the `cpu` lane on every change to the C core and fails when a file
differs from `scripts/ci/tidy-baseline-cpu.json`. It configures exactly what
`TIDY_RATCHET_SETUP_cpu` says; `scripts/ci/tests/test_tidy_lane_container.py`
compares the two lines and the clang-tidy major, and runs in the job's first
step. The container reproduces the hosted measurement byte for byte: for
master `513d2a6fc` the report of `scripts/dev/tidy-lane.sh cpu` and the
`tidy-ratchet-cpu` artifact of hosted run 37011276599 are the same file
(SHA-256 `ffb5ca1819a3…`, 327 translation units, 322 findings).

No hosted runner has `nvcc`, `hipcc` or `icpx` with a full build, so `cuda`,
`hip` and `sycl` are not required checks, and neither is `arm64`. Their
baselines still hold: a change that raises a count shows up the next time the
lane runs, locally or in the nightly run below. A lane that did not run is
never reported as clean.

## Nightly run on the workstation

Until a runner with the device toolchains exists, the lanes without a hosted
job run from a timer on the workstation that has the dev image. The job measures
a clean
clone of `master`, not a working tree:

```bash
git -C ~/.cache/vmafx-tidy-nightly/vmafx fetch --quiet origin master
git -C ~/.cache/vmafx-tidy-nightly/vmafx checkout --quiet --detach origin/master
~/.cache/vmafx-tidy-nightly/vmafx/scripts/dev/tidy-lane.sh \
    --out ~/.cache/vmafx-tidy-nightly/$(date +%F) all
```

Run it from a systemd user timer. The service is a `Type=oneshot` unit whose
`ExecStart=` lines are the three commands above:

```ini
# ~/.config/systemd/user/vmafx-tidy-lanes.service
[Unit]
Description=Measure the clang-tidy lanes on master

[Service]
Type=oneshot
ExecStart=/usr/bin/git -C %h/.cache/vmafx-tidy-nightly/vmafx fetch --quiet origin master
ExecStart=/usr/bin/git -C %h/.cache/vmafx-tidy-nightly/vmafx checkout --quiet --detach origin/master
ExecStart=%h/.cache/vmafx-tidy-nightly/vmafx/scripts/dev/tidy-lane.sh --out %h/.cache/vmafx-tidy-nightly/nightly all
```

```ini
# ~/.config/systemd/user/vmafx-tidy-lanes.timer
[Timer]
OnCalendar=*-*-* 03:30:00

[Install]
WantedBy=timers.target
```

The unit files above are illustrative: `%h` is systemd's home directory, and a
fixed `--out` directory replaces the `$(date +%F)` of the shell form (systemd
does not expand shell substitutions). A non-zero result shows in
`systemctl --user status vmafx-tidy-lanes.service` and the lane logs name the
files. An exit `2` or `3` on `master` means a change landed without its lane
being measured; open a row in [`docs/state.md`](../state.md) and fix the file
or tighten the baseline through `make tidy-lane-write`.
