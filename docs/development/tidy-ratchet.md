<!-- markdownlint-disable MD013 -->
# Whole-tree lint ratchet (ADR-1142)

The ratchet bounds clang-tidy findings for every file in the tree. A file may
never get worse than its committed baseline, and a cleaner file must tighten
its baseline in the same PR. This page is the reference for the rule, the
lanes, the commands and the carve-outs that remain; the day-to-day
measurement workflow is in [measuring the lanes](tidy-lanes.md).

Since [ADR-1142](../adr/1142-whole-codebase-standards.md) the coding standards
apply to every file, and CI enforces that with this ratchet instead of a
touched-files rule. The changed-files job `Tidy Changed` stays as fast
feedback and keeps the `WarningsAsErrors` hard stop; ADR-0141's "a touched file
ends the PR at zero" is unchanged. The ratchet adds the bound on untouched
files.

`cert-err33-c` (an ignored return value of a standard-library call) is in
`WarningsAsErrors` since 2026-10-06 ([ADR-0694](../adr/0694-vmafx-lint-sanitizer-gates.md)
asked for it): the `cpu`, `cuda`, `hip`, `sycl` and `arm64` lanes measure zero
such findings, so a new unchecked `fclose()` or `fputs()` fails `Tidy Changed`
instead of joining the baseline. Handle the result; casting it to `void` is not
handling it ([principles](../principles.md)).

## The rule

`scripts/ci/tidy-ratchet.py` runs clang-tidy over every translation unit in a
`compile_commands.json`, deduplicates diagnostics by
`(path, line, column, check)`, counts `NOLINT` markers with no inline
`ADR-NNNN` citation, and compares the per-file numbers with the committed
baseline `scripts/ci/tidy-baseline-<lane>.json`. A citation counts on the
previous, the same or the next line, or anywhere in the `/* ... */` block
comment that holds the marker.

The rule is *baseline equals measurement*:

| Exit | Meaning | What to do |
| --- | --- | --- |
| `0` | Every file matches its baseline. | Nothing. |
| `2` | A file is above its baseline. | Fix the code; never raise the baseline. |
| `3` | A file is below its baseline. | Tighten it with `make tidy-ratchet-write` and commit the JSON in the same PR. |
| `4` | A compilation, tool or diagnostic-parse failure made the measurement unusable, or a translation unit of the baseline's `measured_sources` was not measured (named as `not measured`): its count is unknown, not 0. | Fix the build, or build the lane's configuration (`make tidy-ratchet-build LANE=<lane>`); the ratchet fails closed. |
| `5` | Usage or I/O error, or a scoped-validation error. | Read the message. |

## Lanes

Each lane has its own compile database and baseline. The numbers are the
`total_warnings` of the committed baselines.

| Lane | Covers | Measured | PR-required | Baseline | Warnings |
| --- | --- | --- | --- | --- | ---: |
| `cpu` | The whole CPU tree | CI job `Tidy Ratchet`; dev container for the baseline | Yes (`Tidy Ratchet`) | `tidy-baseline-cpu.json` | 13 |
| `cuda` | CUDA host and `.cu` kernels | Dev container with `nvcc` | Yes (`Tidy Lane (cuda)`, path-routed) | `tidy-baseline-cuda.json` | 0 |
| `sycl` | SYCL host and kernels | Dev container with `icpx` | Yes (`Tidy Lane (sycl)`, path-routed; see [carve-outs](#carve-outs-still-open-after-adr-1142)) | `tidy-baseline-sycl.json` | 0 |
| `hip` | HIP host and `.hip` kernels | Dev container with `hipcc` | Yes (`Tidy Lane (hip)`, path-routed) | `tidy-baseline-hip.json` | 0 |
| `arm64` | NEON and SVE2 tree | Dev container with the aarch64 cross compiler and `qemu-user` | Yes (`Tidy Lane (arm64)`, path-routed) | `tidy-baseline-arm64.json` | 0 |

The `clang` lane (the fuzz targets and `read_json_model.c` built with clang) is
required as `Tidy Lane (clang)`, and the macOS `metal` lane (the Objective-C++
host code and the Metal-only tests, `tidy-baseline-metal.json`) as `Tidy Metal`.
The `.metal` shader sources have no clang-tidy language mode and are on the
exception list. Measure a lane with `make tidy-lane LANE=<lane>`; see
[measuring the lanes](tidy-lanes.md). A lane that did not run is reported as
*not run*, never as clean.

## Hosted lanes (ADR-2796)

Every lane runs in hosted CI and is a required check, scoped to the files the
lane reads ([ADR-2796](../adr/2796-hosted-tidy-all-lanes.md)):

| Lane | Required context | Where it runs | Selector (`.github/ci-impact.json`) |
| --- | --- | --- | --- |
| `cpu` | `Tidy Ratchet` | the runner, `lint-and-format.yml` | `c_core` |
| `cuda`, `hip`, `sycl`, `arm64`, `clang` | `Tidy Lane (<lane>)` | the digest-pinned dev container, `lint-and-format.yml` | `tidy_<lane>` |
| `metal` | `Tidy Metal` | a macOS runner, `tidy-metal.yml` | `tidy_metal` |

- **Routing.** Each `tidy_<lane>` selector is `own_paths_only`
  ([ADR-1700](../adr/1700-tester-selectors-own-paths-only.md)): it is true when a
  changed file is one of the lane's own sources or tests, the ratchet
  (`scripts/ci/tidy-ratchet.py`), the lane's baseline, the lane scripts,
  `.clang-tidy`, the `Makefile` (which defines the lanes) or the workflow that
  hosts the lane. A change to a file every lane compiles (`core/src/libvmaf.c`)
  selects the `cpu` lane only; the nightly sweep finds what it does to the
  others. A docs-only change selects none. `python3 scripts/ci/plan-ci-impact.py
  --event pull_request --base "$(git merge-base origin/master HEAD)" --head HEAD
  --print` shows what a branch selects.
- **A skipped lane says so.** The leg always starts, so the context always
  reports. When its selector is false it prints
  `::notice::tidy_<lane> not impacted (mode=... reason=...) ... NOT measured`
  and measures nothing; it is never counted as a clean measurement.
- **The pinned container.** The device and cross lanes need `nvcc`, `hipcc`,
  `icpx` and the aarch64 cross compiler, which only the dev container has.
  `.github/actions/tidy-lane/action.yml` names the image by digest and
  `scripts/dev/tidy-lane.sh` measures the lane in it, as a developer does. After
  a `dev/Containerfile` change publishes a new image, bump the digest in the same
  pull request that re-measures the baselines. The package is private; the job
  reads it with the repository token (`packages: read`).
- **Nightly sweep.** `nightly.yml` measures the `cpu` lane and every lane above
  without routing, and `tidy-metal.yml` runs nightly. A failing sweep opens or
  updates the one issue "Tidy ratchet drift on master"
  (`scripts/ci/tidy-drift-issue.sh`), with the failed lanes and the run link.
- **Fixing a red lane.** Fix the code, then tighten the baseline through the
  write command ([scoped write](#tighten-selected-files-with-a-scoped-write),
  `make tidy-lane-write`); never edit a baseline by hand.

### The `cpu` lane

The required context `Tidy Ratchet` in `lint-and-format.yml` is in the
aggregator list ([ADR-0313](../adr/0313-ci-required-checks-aggregator.md)).
Like every required job it always starts and first runs the
[ADR-1140](../adr/1140-ci-impact-planner.md) impact planner
(`scripts/ci/plan-ci-impact.py`, step id `impact`). The install, build and
ratchet steps run only when the planner's `c_core` selector is `true`;
otherwise a `Not impacted` notice satisfies the context. `.clang-tidy`,
`scripts/ci/**` (the ratchet and its baselines) and the workflow file are
CI-authority inputs that force `mode=full`, so a ratchet or baseline edit
always runs the lane.

Write the baseline in the dev container:

```bash
make tidy-lane-write LANE=cpu
```

The job and the container run the same `make tidy-ratchet-build` and
`make tidy-ratchet` targets, so this measures what the job measures and
reproduces its report byte for byte ([ADR-1471](../adr/1471-tidy-lanes-in-dev-container.md)). A baseline
written on a workstation outside the container records that machine's C
library and compilers and fails in CI.

The job uploads `tidy-ratchet-cpu`, the measurement JSON with every
diagnostic, so a difference can be read without a rerun. The nightly workflow
runs the same lane and fails on drift (it used to swallow the full scan with
`|| true`).

Generated sources are skipped. The compile database lists translation units
that Meson generates into the build directory: the `xxd` model embeds
`src/vmaf_v0.6.1.json.c`, `src/brisque_live.model.c` and so on. They are build
products, which [ADR-1142](../adr/1142-whole-codebase-standards.md) exempts, so
the ratchet skips every source and header under `--build-dir`. An in-repo
`build/` (the nightly workflow, and the `core/build` default of
`make tidy-ratchet`) therefore measures the same checked-in files as the
out-of-repo build the required job uses.

### The `cuda`, `sycl` and `hip` lanes

These lanes are measured in the dev container with the device toolchains
(`nvcc`, `hipcc`, `icpx`), so the device bodies and the kernels are parsed and
not the `-ENOSYS` stubs of a host without them:

```bash
make tidy-lane LANE=cuda          # check a lane; hip, sycl, all also work
make tidy-lane-write LANE=cuda    # rewrite its baseline
```

The lane configurations, the reason a host measurement differs and the nightly
run are in [measuring the lanes](tidy-lanes.md). Inside the container the lane
is two `make` targets, which also work on any machine that has the lane's
toolchain, as a look at the numbers rather than a measurement:

```bash
make tidy-ratchet-build LANE=cuda TIDY_RATCHET_BUILD_DIR=~/.cache/vmafx-gpu-tidy/cuda
make tidy-ratchet LANE=cuda TIDY_RATCHET_BUILD_DIR=~/.cache/vmafx-gpu-tidy/cuda
```

!!! warning "Every lane configures with `-Db_lto=false`"
    `core/meson.build` sets `b_lto_threads=4`
    ([ADR-1172](../adr/1172-bound-lto-link-parallelism.md)), which Meson
    renders as GCC's `-flto=4`. clang-tidy parses these compile commands with
    clang, which rejects the argument outright, so every TU comes back as a
    compile failure and the run exits 4.

The build directory may be inside or outside the repository: generated sources
under it (`*.json.c`, HIP `*_hsaco.c`) are skipped either way, and the
committed baselines contain checked-in paths only. The ratchet enforces what
ADR-1290 stated as a precondition (build directory outside the repository), so
an in-tree build can no longer count generated files.

Meson emits the device translation units as custom commands, so each lane runs
a generator between the native database export and the measurement
(`make tidy-ratchet` and `tidy-ratchet-write` do this automatically per lane,
through `TIDY_RATCHET_COMPDB_<lane>`):

| Lane | Generator | Contract test |
| --- | --- | --- |
| `sycl` | `scripts/ci/gen-sycl-compile-commands.py`: Meson emits the SYCL feature TUs as `CUSTOM_COMMAND` rules (`icpx -fsycl`), which `write-compile-commands.py` never sees | `scripts/ci/tests/test_tidy_ratchet_sycl_compdb.py` |
| `cuda`, `hip` | `scripts/ci/gen-gpu-compile-commands.py`: Meson compiles `.cu` and `.hip` through custom targets too. It reads the kernel file from the explicit inputs of each build statement and the compiler from its command, and exits 1 when a statement names a `.cu` / `.hip` file it could not turn into an entry | `scripts/ci/tests/test_gen_gpu_compile_commands.py` |

The `.hip` kernels are parsed by the ROCm toolchain's own clang-tidy through
`scripts/ci/clang-tidy-hip.sh`, because ROCm 10's device headers use a builtin
that stock LLVM 22 rejects.

### The `arm64` lane

[ADR-1283](../adr/1283-whole-tree-ratchet-arm64-lane.md) added the NEON and
SVE2 tree: 32 translation units that compile only on an aarch64 host (the 20
sources under `core/src/feature/arm64/`, `core/src/arm/cpu.c` and the 11
`core/test/test_*_neon.c` parity tests). Before this lane no compile database
held them and the `cpu` lane's "whole tree" stopped at the architecture
boundary.

Like the other lanes it is measured in the dev container, which installs the
cross compiler and `qemu-user` and configures with the in-tree cross file plus
`build-aux/aarch64-linux-gnu-qemu-user.ini`:

```bash
make tidy-lane LANE=arm64
```

By hand, for a look at the numbers on a machine with a cross toolchain:

1. Cross-compile with the in-tree cross file:

    ```bash
    meson setup build-arm64 core --cross-file build-aux/aarch64-linux-gnu.ini \
        -Denable_cuda=false -Denable_sycl=false -Db_lto=false
    ```

2. Generate the codegen outputs. They must exist on disk before clang-tidy
   parses the TUs that include or are them, which is why the `cpu` lane builds
   before it measures. A full `ninja -C build-arm64` does it; these are the only
   two groups needed:

    ```bash
    ninja -C build-arm64 include/vcs_version.h
    ninja -C build-arm64 $(ninja -C build-arm64 -t targets all \
        | sed -n 's/^\(src\/[^:]*\.c\): CUSTOM_COMMAND.*/\1/p')
    ```

3. Measure:

    ```bash
    make tidy-ratchet LANE=arm64 TIDY_RATCHET_BUILD_DIR=build-arm64
    ```

Skipping step 2 is not a quiet inaccuracy: `vcs_version.h` alone takes three
translation units to `clang-diagnostic-error` and the ratchet fails closed with
exit 4.

Prerequisites are an aarch64 cross gcc and a glibc sysroot
(`aarch64-linux-gnu-gcc` plus `aarch64-linux-gnu-glibc` on Arch,
`gcc-aarch64-linux-gnu` plus `libc6-dev-arm64-cross` on Debian or Ubuntu) and
clang-tidy. `make` forwards `--target=$(AARCH64_TARGET)` and
`--sysroot=$(AARCH64_SYSROOT)` to clang-tidy so it parses the
`aarch64-linux-gnu-gcc` compile commands as AArch64; without the target it
reads `<arm_neon.h>` against the host's x86 headers and reports every NEON
translation unit as a compile failure (exit 4). Both default to the cross
package's own paths and are overridable on the `make` command line.

The baseline names the container's cross compiler
(`aarch64-linux-gnu-gcc (Ubuntu 15.2.0-16ubuntu1)`), because the counts depend
on the C compiler's system headers (ADR-1230).

## Tighten selected files with a scoped write

When a full matching CPU build is unavailable,
[ADR-1243](../adr/1243-tidy-scoped-baseline-tightening.md) allows an existing
compilation database to measure and tighten selected source files without
replacing other entries. Run from the repository root after building the
selected targets so generated headers exist:

```bash
python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build \
  --only core/src/thread_pool.c \
  --only core/test/test_thread_pool_backpressure.c \
  --report /tmp/thread-pool-tidy.json --write
```

The rules of a scoped write:

- Every `--only` path must be a translation unit in that database. A
  missing or empty selection, a failed tool, an unparsed diagnostic or a
  compiler error leaves the baseline unchanged.
- The clang-tidy version must exactly match the baseline. Existing checks
  promoted by `WarningsAsErrors` are still counted as warning debt; genuine
  tool failures cannot produce a clean measurement.
- A scoped write may only lower allowances. It rejects every observed
  increase, including in included headers, and preserves all unselected
  source and header entries.
- It removes a selected entry measured at zero, updates aggregate totals, and
  records selected sources, before and after counts and the preceding
  baseline's canonical JSON hash in `scoped_updates`. The original `tus`,
  generator and tool metadata describe the last full measurement, not a new
  whole-tree scan.
- The separate report records the actual measured sources and failures; it
  must not alias the baseline. Replacement is atomic after validation, and
  repeating an unchanged scoped measurement leaves the baseline byte-identical.
- It also drops, from every file it writes, each baseline entry
  (`warnings`, `nolint_uncited`, `measured_sources`) whose file no longer
  exists in the tree, prints each dropped path and records the list under
  `dropped_deleted_files` in `scoped_updates`. A change that deletes a source
  therefore needs no full re-measure of the lane. An entry for a file that
  still exists but was not measured stays: the gate then fails with exit 4
  ("not measured"), because a file the baseline cannot see is unknown, not
  clean. A renamed file drops its old path; the new path enters the lane only
  by being measured, with `--only <new path>`. Run
  `python3 scripts/ci/praetor_tidy_coverage.py --write` afterwards to
  regenerate `.config/clang-tidy/measured-sources.txt`, which is derived
  from the baselines.
- `--only` without `--write` remains diagnostic-only and skips comparison.

Required CI continues to measure the full configured tree; a successful scoped
write cannot stand in for that gate or clear unmeasured debt.

### Locking

Both full and scoped baseline writes require POSIX advisory locking, available
in the Linux CI and Linux or macOS developer lanes. A second writer fails with
exit 5 while the first owns the resolved baseline path; retry after that
process exits. Lock ownership releases on exit, and the empty lock file under
the per-user temporary directory is retained to keep its inode stable. Writers
also reject content drift since measurement and before replacement.

External editors and Git operations do not honor this lock, so keep the
checkout stable during measurement; a successful write does not certify safety
against arbitrary concurrent repository mutation. Unreadable `NOLINT`
source or header files also fail closed instead of clearing their allowance.

## Carve-outs still open after ADR-1142

The 2026-09-02 inventory ([research
digest](../research/2027-lint-carveout-inventory-2026-09-02.md))
found 218 scope restrictions across the lint and CI configuration. ADR-1142
retired the nightly `|| true` and bounded the whole CPU tree; the remaining
rows are owned by the wave that brings the blocking toolchain or build option
to CI:

| Carve-out | Blocker | Owner / plan |
| --- | --- | --- |
| `Tidy Changed` excludes `core/src/cuda/`, `core/src/feature/cuda/`, `core/test/test_cuda_*`, `core/test/test_gpu_picture_pool.c` | CUDA toolkit headers on the hosted runner (`--cuda-host-only` needs them) | `cuda` lane becomes PR-required; retire the `grep -v` lines in the same PR |
| `Tidy Changed` excludes `core/src/sycl/`, `core/src/feature/sycl/` and `core/test/test_sycl*`. The separate `Tidy SYCL` job covers those changed-file globs as a required, fail-closed gate, but no workflow runs `tidy-ratchet.py --lane sycl`, and `core/tools/vmaf_vpl.c` is outside that job's selectors | The CPU ratchet has no complete SYCL/oneVPL compile database; the dedicated oneAPI job measures changed SYCL TUs, not the whole SYCL baseline | Add a required whole-lane SYCL ratchet and include `core/tools/vmaf_vpl.c`; keep `Tidy SYCL` required |
| `Tidy Changed` excludes `core/src/hip/`, `core/src/feature/hip/`, `core/test/test_hip*` | ROCm headers on the hosted runner | `hip` lane becomes PR-required |
| `Tidy Changed` excludes `core/src/feature/arm64/` | No aarch64 compile database on x86 runners | The `arm64` lane measures it in the dev container and has a committed baseline; it is not PR-required yet |
| `Tidy Changed` excludes `core/src/mcp/`, `core/test/test_mcp*`, `core/test/fuzz/`, `core/src/compat/win32/`, `core/tools/vmaf_vpl.c` | Needs `-Denable_mcp=true`, fuzz, libva or MinGW compile databases | Add those TUs to the `cpu`-lane build in CI |
| `.cppcheck-suppressions.txt` per-file suppressions, `.clang-tidy` disabled checks, `.semgrep.yml` path excludes, `pyproject.toml` per-file ignores | None; each is a fix-the-code item | Rework waves; each removal is a ratchet decrease |

## History

- **Baselines at the time ADR-1142 landed (2026-09-02):** cpu 5,241 warnings
  across 281 TUs with 83 uncited NOLINTs; cuda 1,650; sycl 716; hip 1,173
  (whole tree about 8,780). The committed baselines now read as the
  [Lanes](#lanes) table says.
- **`sycl` lane:** before the generator hook existed the lane measured zero
  SYCL translation units and `tidy-baseline-sycl.json` recorded an empty
  backend.
- **`cuda` and `hip` lanes:** the first version of
  `gen-gpu-compile-commands.py` matched `<kernel> | <compiler>` only; once the
  kernel targets listed their headers as dependencies it found no rule at all,
  and both lanes measured the host files alone until 2026-10-02.
- **Out-of-repo build directory:** ADR-1290 first stated it as a precondition;
  the ratchet now enforces it.
