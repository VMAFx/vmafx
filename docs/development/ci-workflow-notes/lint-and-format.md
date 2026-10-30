# Design notes: lint-and-format.yml

These notes were moved out of `.github/workflows/lint-and-format.yml` so the
workflow stays under the 64 KiB input bound of the praetor engine. They are the
workflow's original comments, grouped by job, unchanged.

## general

```text
Static-analysis gate. Runs in parallel with the libvmaf Build Matrix so a
lint failure doesn't block discovering build breakage and vice versa.
ADR-0331 — `ready_for_review` lets the per-job draft gate fire
CI when a draft PR is promoted to ready-for-review.
```

## pre-commit

```text
Resolve LFS blobs (e.g. model/tiny/*.onnx) so pre-commit's final
"changes made by hooks" diff doesn't report pointer-vs-blob noise
against files no hook actually touched.
No cl.exe leg builds on a light-tier pull request (ADR-2169, ADR-3279),
so the static msvcism scan of scripts/dev/preflight.sh is the
required check that sees a C23-only keyword such as `nullptr` in a C
translation unit before master does (#2852). Changed files only: the
whole-tree mode reports files kept off Windows by their meson.build.
pre-commit/action@v3.0.1 is the latest release (Feb 2024) and still
pins actions/cache@v4, which trips the Node.js 20 deprecation
warning (Node 20 removed from runners 2026-09-16). Inline the
three steps it would have run, using actions/cache@v5.
The generated-docs hook validates docs/hardware-reports against
their schema and exits 2 without it (check-hardware-reports.py).
Tooling Tests runs those tests on every pull request and push; a test
runs once in CI (ADR-1568). The hooks stay active at commit time.
HISS-18: a pull request is checked on the files it changes
(`--from-ref <base sha> --to-ref HEAD`); master pushes, release tags,
schedule and dispatch keep `--all-files`. scripts/ci/precommit_scope.py
decides and prints the reason; test_precommit_scope.py proves it.
The independent FFmpeg Patch Stack job owns network replay and
ADR-1140 routing; a full run here would replay on docs-only PRs.
It also runs `make ffmpeg-input-contract` on every pull request,
so the hook of that name is skipped here as well (ADR-1568), the
Licence Provenance job runs check-licence-metadata (ADR-1699), and
the REUSE lint job of reuse.yml runs reuse-lint (ADR-2784).
```

## clang-tidy

```text
Need history for the push-delta diff on master. fetch-depth: 2
is enough for a squash-merge FF (one new commit), but use 0 to
tolerate multi-commit FF pushes without a second fetch step.
Early file-delta probe — if the PR / push touches no C/C++ files,
short-circuit before the apt-install + meson-setup + meson-compile
steps (~5 min on a cold runner). Saves doc-only / Python-only PRs
from paying the heavy lint setup. The body of `Run clang-tidy on
changed files` does its own (more thorough, exclusion-aware) delta
later; this step only asks "is there anything plausible?".
A root commit has no HEAD~1; that is the only expected
failure, so test for it instead of swallowing git's status.
Restore-only; the save below runs only after a successful install.
Caches downloaded .deb archives so a slow package mirror does not eat
into the apt step's timeout. Keyed on runner OS, arch, job, and the
hash of the job's apt package list under .github/apt/tidy-changed.txt.
llvm.sh steps measured max 5.7m over the jobs that run it (2026-10-23; a
Tidy Ratchet run passed 5.2m); 10 min is at least 1.25x the slowest.
gcc from ubuntu-toolchain-r/test for the same reason clang comes
from apt.llvm.org and meson from PyPI: the ubuntu-24.04 image's
newest gcc is 14, and the tree targets C23 / C++26 (ADR-0692).
Leaving gcc at the distro version while pulling the other two
forward was an oversight, not a decision -- and it is not free.
gcc supplies the system headers clang-tidy parses, so the ratchet's
warning counts depend on it; pinning it a major behind every
developer's box makes a ratchet delta unreproducible locally, which
is exactly how ADR-1230 came about.
clang-tidy from apt.llvm.org: the ubuntu-24.04 image tops out at
clang 18, whose libstdc++ has no std::expected, so it cannot even
parse core/src/dict.cpp since ADR-0692 moved the tree to C++26.
meson from PyPI: Ubuntu 24.04 ships meson 1.3.2, which predates `c23`
in c_std (added in meson 1.4.0) and fails configure since ADR-0692.
Invoke the versioned binary below. GitHub's Ubuntu image already
owns the unversioned alternatives group; adding another entry at
the same priority can leave `clang-tidy` resolving to v18, which
cannot parse the tree's C++26 `std::expected` use.
Saves only after the install step succeeded: an install cut off by its
timeout leaves the archive directory empty, and an empty entry under
this immutable key would be restored as a hit on every later run
(an empty 200-byte entry was saved this way on 2026-10-22). Runs even
when a later step fails, so the next run skips the download.
Build first so codegen outputs (vcs_version.h, model JSONs → .c)
exist on disk before clang-tidy parses the translation units.
Hardening per Semgrep `yaml.github-actions.security.run-shell-injection`
(Research-0090, F2): pull every `${{ github.* }}` interpolation into
`env:` and reference the env var with double-quotes in shell. The
`before` SHA, `event_name` enum, and `base_ref` branch name are all
GitHub-controlled and currently safe, but the env-aliasing pattern
forecloses any future case where one of those values gains a shell
metacharacter and makes the data-flow explicit for maintainers.
Collect the delta for both PR and push events so master-post-merge
doesn't re-scan every vendored .c/.cpp (e.g. core/src/svm.cpp)
and surface pre-existing warnings unrelated to the push.

Excluded path families — the shared reason is that the compile
commands in `build/compile_commands.json` don't cover these TUs
(CPU-only + ARM-only build here), so clang-tidy parses them with
default flags and spurious `clang-diagnostic-error` output:
  - core/src/feature/arm64/ — NEON / SVE2, needs a cross
    toolchain. Measured by the ADR-1283 `arm64` ratchet lane:
    `meson setup build-arm64 core --cross-file
    build-aux/aarch64-linux-gnu.ini -Denable_cuda=false
    -Denable_sycl=false -Db_lto=false` then `make tidy-ratchet
    LANE=arm64 TIDY_RATCHET_BUILD_DIR=build-arm64`. Same posture
    as the CUDA / HIP families below: excluded here, bounded
    there.
  - core/src/cuda/ + core/src/feature/cuda/ — require
    `-Denable_cuda=true` meson setup to generate compile_commands
    (follow-up: add a parallel CUDA clang-tidy job once a
    self-hosted CUDA runner lands, backlog T7-3).
  - core/test/test_cuda_*.c + test_gpu_picture_pool.c — GPU-gated
    tests whose build entries only exist under the CUDA build.
    test_gpu_picture_pool.c is the renamed test_ring_buffer.c per
    ADR-0239 (still CUDA-only — it pulls in libvmaf_cuda.h).
  - core/src/sycl/ + core/src/feature/sycl/ +
    core/test/test_sycl* — require `-Denable_sycl=true` and
    the `icpx` compiler; no compile_commands in the CPU-only
    `build/`. Local lint runs against `core/build-sycl/`.
    core/test/test_integer_cambi_sycl.c is the one member of
    that family that does not carry the `test_sycl` prefix, so
    it needs its own line: it is `#if HAVE_SYCL`-gated like the
    rest, meson builds it only under `-Denable_sycl=true`, and
    without a compile command clang-tidy parses it with default
    flags and dies on `'config.h' file not found`
    (clang-diagnostic-error). It is measured by the `Tidy SYCL`
    job instead, which had to name it explicitly: that job
    selects `core/test/test_sycl*.c`, and this file is the one
    member of the family without that prefix, so before it was
    added there it fell between the two jobs and was measured by
    neither. Do NOT cite the `sycl` ratchet lane here as the
    bound: no workflow runs it. `grep -rn 'tidy-ratchet.py --lane'
    .github/workflows/` returns two hits, both `--lane cpu`, and
    the Tidy Ratchet job configures `-Denable_sycl=false`;
    scripts/ci/tidy-baseline-sycl.json is a workstation
    measurement, not a CI gate.
  - core/tools/vmaf_vpl.c — VAAPI integration; needs
    `va/va.h` from libva-dev plus the CUDA/SYCL link path; no
    compile_commands entry in the CPU-only build.
  - core/src/mcp/ + core/test/test_mcp_smoke.c —
    gated by `-Denable_mcp=true`; the CPU-only lint build
    has the umbrella flag off (ADR-0209 audit-first
    scaffold, T5-2). Local lint runs against a build
    configured with `-Denable_mcp=true -Denable_mcp_sse=true
    -Denable_mcp_uds=true -Denable_mcp_stdio=true`.
Local lint on these files runs against `core/build-cuda/`.
`--diff-filter=d` excludes Deleted entries — clang-tidy can't
lint a file that no longer exists on disk; without this filter
the script passes a missing path to clang-tidy and fails with
`clang-diagnostic-error: no such file or directory`.
  - cmd/vmafx-node/bpf/ — eBPF programs. They include `vmlinux.h`,
    which is generated from the running kernel's BTF and does not
    exist on the runner, so clang-tidy cannot parse them at all
    (`clang-diagnostic-error: 'vmlinux.h' file not found`). What it
    does report is an artefact of reading an eBPF object as ordinary
    C++: libbpf's `SEC()` macro identifiers, and programs and maps
    that must keep external linkage for the loader to find them.
    Same posture as the CUDA/SYCL/HIP families above.
  - Paths in scripts/ci/pelorus-mirror-paths.txt — vendored
    verbatim mirror of the Pelorus interop ABI (ADR-1113),
    byte-identical to its single source of truth; cannot absorb
    fork-local lint fixes without breaking the sync guard. The
    shared filter is exact-path based, so an unmanifested
    pelorus_*.cpp remains lint-owned. Fix upstream and re-sync.
  - core/src/metal/objc_handle.h — Objective-C++ only: it stops at
    `#error` unless __cplusplus is defined and includes `<bit>`
    and <Metal/Metal.h>, so a C parse of it fails by construction.
    The macOS Metal lane reads it (tidy-baseline-metal.json,
    docs/development/tidy-lanes.md); this CPU-only job cannot.
  - core/src/feature/ff_math.h + core/src/feature/ciede_ff_math.h —
    C++ headers (`<cstddef>`, `namespace vmaf_ciede_ff`) that only
    the SYCL, HIP and Metal ciede twins include. With no compile
    command of their own, clang-tidy reads them with the command of
    a neighbouring C file and stops at `'cstddef' file not found`.
    The lanes that compile their includers read them.
  - core/test/vmafx_fixture_util.h — a test helper that stops at
    `#error` unless core/test/meson.build defines VMAFX_TEST_YUV_DIR.
    With no compile command of its own it is parsed with a
    neighbouring file's command and fails by construction; the cpu
    lane of the Tidy Ratchet reads it through the tests that include it.
  - core/test/test_adm_small_border.c +
    core/test/test_adm_wide_rounding.c — backend-neutral source
    names, but meson builds them only as test_cuda_adm_* and
    test_hip_adm_*, so the CPU build has no compile command for
    them. CI has no GPU tidy lane; lint them against a CUDA or HIP
    build with `make tidy-ratchet LANE=cuda` / `LANE=hip`.
  - .config/hiss/testdata/ — the HISS rule engine's own fixtures.
    Each rule ships a positive/negative/gap triple whose planted
    defect *is* the fixture: HISS-08's positive case calls `gets`
    and HISS-04's calls a 76-line function, so clang-tidy reports
    `use of undeclared identifier 'gets'` and
    `readability-function-size` by construction. meson builds none
    of them, so they have no compile command either and are parsed
    with a command inferred from an unrelated TU. Nothing in the
    shipped library is covered by linting them, and cleaning them
    would destroy the fixtures the gates in `standards-gate.yml`
    replay. `(^|/)testdata/` is already the tree-wide fixture
    exclusion in `.pre-commit-config.yaml`; this is the same rule
    applied to the changed-files tidy lane.
  - compat/python-vmaf/matlab/ is NOT excluded: the MEX sources include the
    MATLAB SDK's `mex.h` / `matrix.h`, which no runner has, so the step
    `Generate compile_commands.json` appends entries that point at the
    self-authored stubs in `scripts/ci/lint-stubs/matlab/`
    (`scripts/ci/gen-mex-compile-commands.py`, the `cpu` lane does the
    same; T-TIDY-MATLAB-MEX-UNMEASURED-2026-09-22).

One filter shared by every trigger path below, so adding a family
is a single edit.
Initial push / new branch: fall back to HEAD~1..HEAD if a
parent exists, otherwise skip (no meaningful delta).
workflow_dispatch and other triggers: scan full tree (slow
but deliberate — operator asked for it).
Q-341: a changed file this CPU build has no compile command for is
deferred to the clang-tidy lane whose measured_sources
(scripts/ci/tidy-baseline-<lane>.json) lists it, and the lane is
named; with no such lane the job fails and names the file. A
header is deferred the same way when none of the units that
include it has a command here (scripts/ci/tidy_changed_route.py,
planted cases in scripts/ci/tests/test_tidy_changed_route.py).
Drop empty lines — the `tr '\n' ' ' | tr ' ' '\n'` round-trip
leaves a trailing blank that parallel would pass to clang-tidy
as an empty filename, which clang resolves to the workspace
directory and reports as a clang-diagnostic-error.
```

## clang-tidy-ratchet

```text
The planner is fail-closed: .clang-tidy, scripts/ci/** (ratchet +
baselines) and this workflow are CI-authority inputs that force
mode=full, so a ratchet or baseline edit always runs the lane.
Restore-only; the save below runs only after a successful install.
Caches downloaded .deb archives so a slow package mirror does not eat
into the apt step's timeout. Keyed on runner OS, arch, job, and the
hash of the job's apt package list under .github/apt/tidy-ratchet.txt.
llvm.sh steps measured max 5.7m over the jobs that run it (2026-10-23; a
Tidy Ratchet run passed 5.2m); 10 min is at least 1.25x the slowest.
See the ADR-1230 note on the changed-files lane above: gcc moves
forward with clang and meson rather than staying at the distro's.
libvpl-dev: the dev container that writes the baseline has it, and
with it Meson builds test_vmaf_vpl_decode_ceiling and vmaf_vpl_core.c,
two translation units of the baseline. The ratchet fails on a
baseline translation unit this build does not measure.
Saves only after the install step succeeded: an install cut off by its
timeout leaves the archive directory empty, and an empty entry under
this immutable key would be restored as a hit on every later run
(an empty 200-byte entry was saved this way on 2026-10-22). Runs even
when a later step fails, so the next run skips the download.
The cpu lane is defined once, in the Makefile (ADR-1471): its
compilers and options (TIDY_RATCHET_COMPILERS_cpu,
TIDY_RATCHET_SETUP_cpu, gcc-15, -Db_lto=false, -Denable_dnn=disabled)
and its extra compile commands (TIDY_RATCHET_COMPDB_cpu, the MATLAB
MEX sources). This job runs the same two targets
scripts/dev/tidy-lane.sh runs in the dev container, so both measure
the same translation units (T-TIDY-RATCHET-UNMEASURED-AS-CLEAN-2026-10-06).
```

## clang-tidy-sycl

```text
Mirrors the main clang-tidy job's PR/push/dispatch fan-out but
keeps only SYCL TUs (the inverse of the exclusion list there).
Context values are passed through env vars (not inline expressions)
to prevent shell injection (github-actions shell-injection rule).
Restore-only; the save below runs only after a successful install.
Caches downloaded .deb archives so a slow package mirror does not eat
into the apt step's timeout. Keyed on runner OS, arch, job, and the
hash of the job's apt package list under .github/apt/tidy-sycl.txt.
llvm.sh steps measured max 5.7m over the jobs that run it (2026-10-23; a
Tidy Ratchet run passed 5.2m); 10 min is at least 1.25x the slowest.
Minimal oneAPI install — only the DPC++/C++ compiler component.
The full base toolkit (~3 GiB) is unnecessary; we only need
<sycl/sycl.hpp> on disk for clang-tidy path resolution.
clang-tidy-22 is NOT in the Ubuntu 24.04 archive — the image tops
out at clang-tidy-18 — so it has to come from apt.llvm.org, exactly
as the CPU `clang-tidy` and `clang-tidy-ratchet` jobs above already
do. Without this the apt install aborts with "Unable to locate
package clang-tidy-22" and this job can never execute.
libze-dev is the Level Zero loader *development* package.
core/src/meson.build calls cc.find_library('ze_loader'), which
emits a literal `-lze_loader`; ld resolves that against the
unversioned libze_loader.so symlink ONLY. The runtime SONAME
libze_loader.so.1 shipped by libze1 is invisible to -l, so
libze1 alone does NOT fix this — do not "simplify" it to libze1.
oneAPI does not supply the loader either: this step installs
intel-oneapi-compiler-dpcpp-cpp and sources setvars.sh before
meson setup, and -lze_loader still fails without libze-dev.
Package name is release-specific: `libze-dev` on 24.04 noble
(universe, already enabled); `level-zero-dev` does NOT exist there.
meson from PyPI: Ubuntu 24.04 ships meson 1.3.2, which predates `c23`
in c_std (added in meson 1.4.0) and fails configure since ADR-0692.
Saves only after the install step succeeded: an install cut off by its
timeout leaves the archive directory empty, and an empty entry under
this immutable key would be restored as a hit on every later run
(an empty 200-byte entry was saved this way on 2026-10-22). Runs even
when a later step fails, so the next run skips the download.
Source oneAPI env and configure a SYCL build tree. icpx is the
required compiler under -Denable_sycl=true.
Build only the generated version header so include/vcs_version.h
exists on disk before clang-tidy parses TUs, without compiling
untouched files across the whole tree.
Meson 1.12 no longer writes the native database itself. Export
only C/C++ compiler rules before appending the custom SYCL TUs.
Synthesise compile_commands.json entries for the SYCL .cpp TUs
that meson emits as CUSTOM_COMMAND rules (icpx -fsycl) rather
than standard cpp_COMPILER rules. Without these entries clang-tidy
falls back to default flags and cannot resolve project headers.
The file list is PR-controlled (file names), so it reaches the shell
through env, never as an inline expression (same rule as the detect
step above and the markdown job's MD_FILES).
Q-342: the wrapper parses every file as C++20. A header no C++
source includes (core/src/sycl/vmafx_sycl_internal.h, C11
atomics) is linted through the C units of build-sycl that include
it instead (scripts/ci/tidy_changed_route.py sycl).
```

## cppcheck

```text
ADR-0359 — pilot job for the ARC self-hosted runner pool.
Variable `ARC_RUNNERS_ENABLED=true` flips this job to the in-cluster
`arc-runners` scale set; default `false` keeps GitHub-hosted. If the
ARC pool is degraded, flip the var off and re-trigger CI. A pull
request from a fork never reaches the pool (ADR-3286).
Measured max 25.1m (2026-10-23); 45 min covers step caps (5+5+5) plus 1.25x 24m rest (45m).
Restore-only; the save below runs only after a successful install.
Caches downloaded .deb archives so a slow package mirror does not eat
into the apt step's timeout. Keyed on runner OS, arch, job, and the
hash of the job's apt package list under .github/apt/cppcheck.txt.
Measured max 1.1m (2026-10-23); 5 min is at least 1.25x the slowest healthy run.
meson from PyPI: Ubuntu 24.04 ships meson 1.3.2, which predates `c23`
in c_std (added in meson 1.4.0) and fails configure since ADR-0692.
Saves only after the install step succeeded: an install cut off by its
timeout leaves the archive directory empty, and an empty entry under
this immutable key would be restored as a hit on every later run
(an empty 200-byte entry was saved this way on 2026-10-22). Runs even
when a later step fails, so the next run skips the download.
Build first so codegen outputs (model JSONs → .c) exist on disk.
cppcheck parses compile_commands.json literally and errors on
any missing translation-unit file.
argv arrays: cppcheck 2.19 does not undo the shell quoting of a
`command` entry and drops meson's quoted string defines
(VMAFX_TEST_MODEL_DIR and friends), which the tests require.
```

## python-lint

```text
The mypy delta gate compares HEAD with origin/master and checks
the same files at their merge base in a disposable worktree.
Pull requests compare with the fetched target branch. A master
push compares with the exact pre-push tip so post-merge CI still
checks the Python paths that landed instead of selecting nothing.
```

## registry-validate

```text
Resolve LFS blobs so the sha256 cross-check sees real ONNX
bytes, not pointer files.
```

## docs-lint

```text
20, not 10: `mkdocs build --strict` over this doc tree measured
9m31s, 9m21s and 5m21s on the last three green runs, and on
ubuntu-26.04 it crosses 10 minutes, so the job was cancelled at the
ceiling mid-build and the aggregator read that as a failure. The
build itself passes with zero warnings; locally it takes about
3m20s. Sibling jobs in this file already sit at 25 and 30 minutes.
The master push checks the render, whose order is read from
history (ADR-2197); a pull request does not. Not
`push && 0 || 1`: 0 is falsy in an expression, so that form
always gave 1 and the render check found a shallow history.
make docs-fragments-check validates docs/hardware-reports against
their schema (jsonschema, check-hardware-reports.py) and reads
docs/credits.yaml (PyYAML, scripts/docs/credits_lib.py, ADR-2485).
A pull request carries fragments, not the render (ADR-2197); the
render lands as its own render-only pull request (ADR-3295), so a
master tip may wait for it. The check passes a current render,
warns while one is owed for less than 24 hours, and is red for an
older owed render or a last render that is not the render of its
fragments.
make docs-fragments-check above runs without vl-convert-python and
compares the chart SVGs with their recorded hashes; this step
renders them again with the hash-locked renderer (ADR-1508).
Reads the built site's index; the checker's own tests run in
Tooling Tests (ADR-1568).
```

## licence-provenance

```text
ADR-1699 — the root licence files and every package manifest's licence
field against the files (scripts/ci/check_licence_metadata.py, standard
library only). This job owns the check in CI; the pre-commit job skips
its hook (ADR-1568).
```

## buf

```text
The protobuf contract under proto/ (ADR-2350) is linted with buf.yaml's
STANDARD rules and compared with the base branch for wire and JSON
breaking changes (WIRE_JSON). buf is the release build-config.env pins
as BUF_VERSION, installed with `go install`, which the Go checksum
database verifies. Full history: `buf breaking` reads the base branch
from the local clone. On a master push the comparison is against the
previous master commit.
Runs after a failed lint too, so one run reports both kinds of finding.
A manual dispatch has no base commit and compares with master.
```

## spectral

```text
Lints api/openapi/vmafx-server-v1.yaml with Spectral's spectral:oas
ruleset (.spectral.yaml, ADR-3331). The standalone release binary is
downloaded at SPECTRAL_VERSION and checked against SPECTRAL_SHA256, both
from build-config.env; no npm dependency tree is installed.
```

## markdownlint

```text
Full history so the push-delta diff against `before` works for
multi-commit FF pushes; PR mode uses origin/${base_ref}...HEAD.
Hardening per Semgrep `yaml.github-actions.security.run-shell-injection`:
pull every `${{ github.* }}` interpolation into `env:` and reference
via env vars. Same posture as the clang-tidy job above.
A failure of git or of grep (status 2 or more) fails the step.
grep's status 1 only means "no line matched", which is an empty
file list and not an error, so keep() maps exactly that status.
Restrict to the tracked markdown corpus (docs/ + changelog.d/
+ top-level *.md). Upstream vendored trees and test fixtures
are excluded via the leading filter; the .markdownlintignore
path would also work but the explicit corpus is clearer.
workflow_dispatch — scan the full corpus. Operator-explicit.
Newline-delimit for the next step (one path per line, no IFS surprises).
```

## trivy

```text
Scan repository Dockerfiles / IaC configs and filesystem dependencies
for CVEs and secrets (ADR-3330). Trivy is the release build-config.env
pins as TRIVY_VERSION, downloaded and verified against TRIVY_SHA256.
```
