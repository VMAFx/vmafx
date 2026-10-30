# Design notes: tests-and-quality-gates.yml

These notes were moved out of `.github/workflows/tests-and-quality-gates.yml` so
the workflow stays under the 64 KiB input bound of the praetor engine. They are
the workflow's original comments, grouped by job, unchanged.

## general

```text
Top-level CI — orchestrates Netflix golden gate (ADR-0024 — must always pass)
and the cross-backend numerical-correctness check. The pre-existing
`libvmaf-build-matrix.yml` handles the cross-platform build matrix; this file
owns the *correctness* gate.
Tests — Netflix Golden / Sanitizers / Tiny AI / Coverage.
The display name is deliberately short: GitHub's badge.svg paints it
into the README status badge. See ADR-1227.
ADR-0331 — `ready_for_review` lets the per-job draft gate fire
CI when a draft PR is promoted to ready-for-review.
No trigger-level paths-ignore: every required job of this workflow
must report on every pull request (ADR-3293), and each job selects
its own work through the CI tier and the impact planner (ADR-2169,
ADR-1140). ADR-0341's docs-only paths-ignore no longer exists.
```

## tier

```text
===== Netflix golden-data gate (ADR-0024) =====
The 3 Netflix CPU test pairs (1 normal + 2 checkerboard) are the source of
truth for VMAF correctness. They MUST pass on every PR. No skips, no excuses.
```

## netflix-golden

```text
Measured max 42.4m (2026-10-23); step caps sum 80 (5+28+5+22+20), limit 85 covers sum plus 1.25x rest.
Restore-only; the save below runs only after a successful install.
Caches downloaded .deb archives so a slow package mirror does not eat
into the apt step's timeout. Keyed on runner OS, arch, job, and the
hash of the job's apt package list under .github/apt/netflix-golden.txt.
Measured max 21.8m (2026-10-23); 28 min is at least 1.25x the slowest healthy run.
pip unpacks every wheel it downloads into $TMPDIR before it
installs it, so the staging area — not just the install target —
has to be on disk. On the ubuntu-26.04 image /tmp is a RAM-backed
tmpfs and systemd v258 puts a per-user quota on it (80% of the
mount), which is what produced "[Errno 122] Disk quota exceeded".
RUNNER_TEMP is on the runner's disk and is emptied per job.
meson from PyPI: Ubuntu 24.04 ships meson 1.3.2, which predates `c23`
in c_std (added in meson 1.4.0) and fails configure since ADR-0692.
Saves only after the install step succeeded: an install cut off by its
timeout leaves the archive directory empty, and an empty entry under
this immutable key would be restored as a hit on every later run
(an empty 200-byte entry was saved this way on 2026-10-22). Runs even
when a later step fails, so the next run skips the download.
Build inside core/ so the CLI lands at core/build/tools/vmaf
— python/vmaf/__init__.py hardcodes that relative path when no
external vmaf binary is configured via vmaf_path() / vmafexec_path().
Restore-only; the save is split out below and gated on success().
The combined actions/cache action saves from a post-job step that runs even
when the job was CANCELLED or FAILED, and the key is content-hashed over
python/test/*_test.py, so any PR touching a Python test gets its own key. A
run cancelled part-way through then published a partially-populated
python/test/resource tree under that fresh key, and every later run on the
branch hit it EXACTLY. download_reactively (compat/python-vmaf/config.py)
re-fetches a fixture only when the file is ABSENT, so a short-but-present
file is never repaired. See the same split in build.yml.
Cache the entire python/test/resource tree — pytest's
download_reactively (python/vmaf/config.py) populates it
lazily and re-uses any pre-existing files. Sized at ~50 MB.
Bump the version suffix when test files reference new
fixture URLs. `restore-keys` lets stale caches partially
populate the dir; missing files re-download lazily.
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
The 3 pairs live in python/test/resource/yuv/ — see D24.
Hardcoded CPU score assertions live in the python/test/*_test.py files.

Run only the D24 normal golden pair. The broader quality/feature
suites are covered elsewhere; this gate exists to protect the
Netflix CPU golden assertions, not to run every Python assertion.
The checkerboard test covers the 10-px and 1-px distorted
Netflix golden pairs. Keep it isolated so a slow 1080p golden
run reports the actual failing surface instead of masking the
normal-pair result.
success() only: a cancelled or failing run must never publish a
partial fixture tree under this key. Placed after both golden pairs.
ADR-0726: vulkan-vif-cross-backend, vulkan-parity-matrix-gate, and
vulkan-vif-arc-nightly jobs deleted (Vulkan backend removed).
===== Upstream Consumer ABI Parity Gate (Issue #2473, ADR-3339) =====
```

## compat-abi-parity

```text
Restore-only; the save below runs only after a successful install.
Caches downloaded .deb archives so a slow package mirror does not eat
into the apt step's timeout. Keyed on runner OS, arch, job, and the
hash of the job's apt package list under .github/apt/compat-abi-parity.txt.
Measured max 1.0m (2026-10-23); 10 min is at least 1.25x the slowest healthy run.
Saves only after the install step succeeded: an install cut off by its
timeout leaves the archive directory empty, and an empty entry under
this immutable key would be restored as a hit on every later run
(an empty 200-byte entry was saved this way on 2026-10-22). Runs even
when a later step fails, so the next run skips the download.
Exit 1 (a difference) is the proof; exit 2 (cannot run) fails the step.
```

## sanitizers

```text
ADR-0359 — ARC self-hosted runner pilot (same pattern as cppcheck in
lint-and-format.yml). Variable `ARC_RUNNERS_ENABLED=true` routes this
job to the in-cluster `arc-runners` scale set; default `false` keeps
GitHub-hosted ubuntu-26.04. If the ARC pool is degraded, set the
variable to false and re-trigger CI to fall back instantly. A pull
request from a fork never reaches the pool (ADR-3286).
Measured max 19.0m (2026-10-23); 30 min is at least 1.25x the slowest healthy run.
Restore-only; the save below runs only after a successful install.
Caches downloaded .deb archives so a slow package mirror does not eat
into the apt step's timeout. Keyed on runner OS, arch, job, and the
hash of the job's apt package list under .github/apt/sanitizers.txt.
Measured max 4.6m (2026-10-23); 10 min is at least 1.25x the slowest healthy run.
LLVM 22 from apt.llvm.org: the ubuntu-24.04 image tops out at
clang 18.1.3, which accepts -std=c++26 but ships a libstdc++
without std::expected, so core/src/dict.cpp fails to build
(ADR-0692 moved the tree to C23 + C++26). Take the toolchain
forward rather than degrading the standard. llvm.sh adds the
repo and runs its own apt-get update.
meson from PyPI: Ubuntu 24.04 ships meson 1.3.2, which predates `c23`
in c_std (added in meson 1.4.0) and fails configure since ADR-0692.
Saves only after the install step succeeded: an install cut off by its
timeout leaves the archive directory empty, and an empty entry under
this immutable key would be restored as a hit on every later run
(an empty 200-byte entry was saved this way on 2026-10-22). Runs even
when a later step fails, so the next run skips the download.
ADR-0347: under UBSan, suppress the `function` check via
`-fno-sanitize=function`. The pre-existing K&R-prototype
`static char *test_X()` pattern across ~50 minunit-style
harness files (`core/test/test.h` + every `test_*.c`)
trips UBSan's function-pointer-type check unconditionally;
rewriting all 50 files to `(void)` is a separate T7-5-style
sweep PR. The suppression preserves UBSan signal on
library code while skipping the harness-only UB.
The suite's Go-running tests find their modules in the cache: the
download happens here, retried, and fails here when it cannot succeed
(scripts/ci/prefetch_go_modules.py, as the user the suite runs as).
ADR-0347: replace the previous `--suite=unit` invocation
(which matched zero tests because no `test()` call in
`core/test/meson.build` carries a `suite: 'unit'` tag).
We now run the full C unit-test set with a per-sanitizer
deselect list — each excluded test corresponds to a real
defect tracked in `docs/state.md` and removed from the list
when the underlying bug-fix PR lands. Companion research
digest: `docs/research/0090-sanitizer-matrix-test-scope.md`.
ASAN_OPTIONS / TSAN_OPTIONS allocator_may_return_null=1:
ASan's and TSan's malloc interceptors abort by default on
allocations they cannot fulfil (e.g. the intentionally-oversized
alloc in test_gpu_picture_pool_uaf which exercises the NULL-return
failure path of vmaf_gpu_picture_pool_init). With this flag the
sanitizer runtime returns NULL instead of aborting, matching the
POSIX malloc contract the test relies on.
ASan deselects: test_model (svm.cpp parser
alloc-too-big on malformed JSON) — pre-existing
ASan/LSan defects revealed by PR #758's introspect-based
test enumeration (the prior `meson test --list` parser
returned empty in CI's meson 1.3.2 so these tests were
never dispatched and the leaks went unnoticed). Tracked
in `docs/state.md` as T-SANITIZER-DEFECTS-REVEALED-758.
test_float_ms_ssim_min_dim was removed from this list
after confirming it passes cleanly under ASAN_OPTIONS=
detect_leaks=1 (the invoke_init helper already calls
fex->close() + free(priv) on every code path).
UBSan deselects: test_model (NULL-to-nonnull memcpy
in svm.cpp parser — same defect as ASan exclusion
surfaced via a different sanitizer angle). Tracked as
T-SANITIZER-DEFECTS-REVEALED-758.
test_cambi was removed from this list after PR #761
(merged 2026-05-11) added a runtime __builtin_cpu_supports
AVX2 gate to test_calculate_c_values_scalar_avx2_parity,
eliminating the SIGILL on non-AVX2 CI runners; confirmed
clean under UBSan on current master.
TSan deselects: test_model (same defect as ASan/UBSan
exclusion above).
test_framesync was removed from this list after PR #548
(merged 2026-05-09) fixed the SAN-FRAMESYNC-MUTEX-DOMAIN
mutex-domain mismatch; nightly TSan job was green on
2026-05-09 and 2026-05-10 (state.md entry).
`meson test --list` output format varies across meson
versions (1.3.x produces empty output for some build
configurations; 1.11.x emits one test per line). Use
`meson introspect --tests` instead — its JSON output is
stable and machine-parseable across all supported meson
versions. Each test object has `name` and `suite[]` fields
which we join into the canonical `libvmaf:<name>` form
that meson test dispatch expects.
===== DNN / tiny-AI gate =====
Builds libvmaf with --enable_dnn (ONNX Runtime C API), runs the dnn
meson suite, and exercises the ai/ Python package's unit tests. Does
NOT require ORT at configure time — stub branch is exercised when the
ubuntu runner doesn't have onnxruntime available via pkg-config.
```

## dnn

```text
The limit is the apt step's 20-minute cap plus 1.25 times the longest
healthy run of the other steps: 27.4 min over 38 successful jobs
(2026-10-22), 20 + 34.3 = 54.3. A 45-minute limit left the build too
little after a slow mirror. scripts/ci/check_job_timeout_budget.py holds
every job limit at or above its step caps' sum (ADR-3367).
Restore-only; the save below runs only after a successful install.
Caches downloaded .deb archives so a slow package mirror does not eat
into the apt step's timeout. Keyed on runner OS, arch, job, and the
hash of the job's apt package list under .github/apt/tiny-ai.txt.
Measured max 191 s on fast mirrors, 11m51s on a slow one (2026-10-22);
20 min is at least 1.25x the slowest healthy run.
Slow mirror downloads took up to 11m51s on healthy runs (#2217,
2026-10-22); an 8-minute limit cancelled jobs 119820120253,
119824997083, 119828883252 and 119836022519 mid-download.
apt's retries and 30 s timeouts below end a real stall.
meson from PyPI: Ubuntu 24.04 ships meson 1.3.2, which predates `c23`
in c_std (added in meson 1.4.0) and fails configure since ADR-0692.
Saves only after the install step succeeded: an install cut off by its
timeout leaves the archive directory empty, and an empty entry under
this immutable key would be restored as a hit on every later run
(an empty 200-byte entry was saved this way on 2026-10-22). Runs even
when a later step fails, so the next run skips the download.
Pinned to 1.22.0. MS stopped publishing Linux tgz artifacts
for patch releases (1.22.1, 1.22.2 are NuGet-only on the
releases page), so v1.22.0 is the last 1.22 line with a
functioning onnxruntime-linux-x64-${VERSION}.tgz. Bumping to
1.23.x or 1.24.x is a separate decision since the dnn loader
is version-sensitive.
Install a pkg-config file since the tarball ships none.
Both the venv and pip's staging area live under RUNNER_TEMP, not
/tmp. On the ubuntu-26.04 image /tmp is a RAM-backed tmpfs with a
per-user quota (systemd v258 enables one on a tmpfs /tmp), and
`pip install -e ai` pulls torch, torchvision and the nvidia-*-cu13
wheel set — several gigabytes. Moving the venv alone is not enough:
pip downloads and unpacks each wheel into $TMPDIR first and only
then extracts it into the venv, so with TMPDIR unset the whole
wheel set still lands on the tmpfs and pip dies with
"[Errno 122] Disk quota exceeded". RUNNER_TEMP is on the runner's
disk and is emptied per job.
test_e2e_frame_to_score.py scores the src01 pair; md5-checked.
The binary this job built: the e2e and CHUG smoke tests run it
instead of skipping (ADR-1528). -rs prints every remaining skip
with its reason.
Suite mini-retrain (ADR-1898): the retrain tooling end to end on a generated
corpus, with the vmaf CLI this job built. Any skip fails the step.
ADR-0174 / T5-3b: enforce that every quantised model in the
registry stays inside its per-model PLCC drop budget. Skips
entries whose `quant_mode == "fp32"` (nothing to gate).
Same reason as the venv step above: pip stages wheels in $TMPDIR.
===== RC1 external tester report package =====
```

## rc1-tester-report

```text
===== Test-suite registry + tooling suite (ADR-1528) =====
`.github/test-suites.json` maps every tracked test file to one suite and
every suite to the required checks that run it. This job fails when a test
file lies outside every suite (tools/vmaf-tune/tests and 60 script tests
once ran nowhere) and runs the tooling suite: every Python and shell test
under scripts/, dev/scripts/, ffmpeg-patches/test/, testdata/ and two tool
test directories. The tests read workflows, Dockerfiles and Meson files as
well as the scripts under test, so no path routing could prove a change
irrelevant: the job always runs.
```

## tooling-tests

```text
Measured max 23.2m (2026-10-23); 35 min is at least 1.25x the slowest healthy run.
Full history: check-source-adr-citations.py verifies the commits
that its retired-ADR records name (`git cat-file -e`), and
test_repository_registry_is_current runs it on this tree. A
shallow clone fails that test and skips the silent-revert history
test.
Restore-only; the save below runs only after a successful install.
Caches downloaded .deb archives so a slow package mirror does not eat
into the apt step's timeout. Keyed on runner OS, arch, job, and the
hash of the job's apt package list under .github/apt/tooling-tests.txt.
Measured max 4.8m (2026-10-23); 10 min is at least 1.25x the slowest healthy run.
package-build first: the docs half of the lock has sdists that build
without isolation (the Docs job installs the docs lock the same way).
test_cppcheck_posix_model.py drives the distribution's cppcheck, the
one the Cppcheck job installs.
Saves only after the install step succeeded: an install cut off by its
timeout leaves the archive directory empty, and an empty entry under
this immutable key would be restored as a hit on every later run
(an empty 200-byte entry was saved this way on 2026-10-22). Runs even
when a later step fails, so the next run skips the download.
The lock installs clang-format at the pre-commit hook's pin; naming it
makes the API generator's format test fail instead of skip when the
installed binary is another major (hosted images ship older ones).
```

## python-package-tests

```text
ADR-1528 — required and in strictMustReport: the impact plan may skip
the work, but every leg runs and reports.
One install step per leg: the lock checker
(scripts/ci/check_python_dependency_locks.py) accepts only literal lock
paths that the manifest owns, never a matrix variable.
===== vmaf-tune suite (ADR-1528) =====
Ten vmaf-tune tests drive a real `vmaf` (ADR-0543 backend enforcement and
V5-1) on the golden pair. Rather than build libvmaf a second time, this job
takes the CLI MCP Smoke builds (artifact `vmaf-cli-mcp`, uploaded right
after its build, so a failing MCP test does not withhold it).
```

## vmaf-tune-tests

```text
Sized as step caps (5+20+5 = 30) plus 1.25x measured 2.2m uncapped run (2026-10-23); 35 min covers sum.
Restore-only; the save below runs only after a successful install.
Caches downloaded .deb archives so a slow package mirror does not eat
into the apt step's timeout. Keyed on runner OS, arch, job, and the
hash of the job's apt package list under .github/apt/vmaf-tune-tests.txt.
Measured max 15.1m (2026-10-23); 20 min is at least 1.25x the slowest healthy run.
The two real-x265 integration tests (test_codec_adapter_x265.py,
test_codec_adapter_x265_two_pass.py; ADR-1565) encode with the
distribution ffmpeg, which links libx265 (ffmpeg 8.0.1 on 26.04).
Saves only after the install step succeeded: an install cut off by its
timeout leaves the archive directory empty, and an empty entry under
this immutable key would be restored as a hit on every later run
(an empty 200-byte entry was saved this way on 2026-10-22). Runs even
when a later step fails, so the next run skips the download.
Opt in to the real ffmpeg/libx265 tests (ADR-1565).
The registry names the files (ADR-1568): a file another suite owns
runs in that suite's job, not here. A test that skips because it
cannot reach the binary or the golden YUVs fails the job: those are
this job's inputs, not optional; so are ffmpeg and its libx265.
```

## mcp-smoke

```text
The limit is the apt step's 20-minute cap plus 1.25 times the longest
healthy run of the other steps, most of it the libvmaf build: 22.1 min
over 44 successful jobs (2026-10-22), 20 + 27.6 = 47.6, rounded up to
Tiny AI's 55. A 35-minute limit cancelled job 119986586497 in the build
after a 19m59s apt step. scripts/ci/check_job_timeout_budget.py holds
every job limit at or above its step caps' sum (ADR-3367).
Restore-only; the save below runs only after a successful install.
Caches downloaded .deb archives so a slow package mirror does not eat
into the apt step's timeout. Keyed on runner OS, arch, job, and the
hash of the job's apt package list under .github/apt/mcp-smoke.txt.
Measured max 57 s on fast mirrors, 11m51s on a slow one (2026-10-22);
20 min is at least 1.25x the slowest healthy run.
Slow mirror downloads took up to 11m51s on healthy runs (#2217,
2026-10-22); an 8-minute limit cancelled jobs 119820120253,
119824997083, 119828883252 and 119836022519 mid-download.
apt's retries and 30 s timeouts below end a real stall.
meson from PyPI: Ubuntu 24.04 ships meson 1.3.2, which predates `c23`
in c_std (added in meson 1.4.0) and fails configure since ADR-0692.
Saves only after the install step succeeded: an install cut off by its
timeout leaves the archive directory empty, and an empty entry under
this immutable key would be restored as a hit on every later run
(an empty 200-byte entry was saved this way on 2026-10-22). Runs even
when a later step fails, so the next run skips the download.
Mirrors the local-lint configuration documented in
lint-and-format.yml (core/build-mcp/ with all four
enable_mcp* flags) so this lane sees identical symbol
availability. Disabling CUDA / SYCL keeps the build
ubuntu-hosted-runner-only — no GPU toolchain required.
tar keeps the executable bit and the RUNPATH layout
($ORIGIN/../src) that an artifact zip would lose (ADR-1528).
The SONAME chain only (files and symlinks): a glob would also take
Meson's object directory libvmaf.so.<version>.p/.
ADR-0209 / T5-2: pins the -ENOSYS contract on every public
vmaf_mcp_* entry point so a future runtime PR (T5-2b) cannot
silently wire a transport without flipping the expectations.
The test is registered as `test_mcp_smoke` in core/test/
meson.build (no suite tag — it lands in the default suite).
The libvmaf compat checks run here too, because only this build
has the vmaf_mcp_* compat functions (ADR-2094): their conformance
against the engine's bodies and the export list with them.
The pyproject ships a `dev` extras group (pytest +
pytest-asyncio + ruff + mypy). There is no `[test]` extras —
`dev` is the right name. Editable install so test_smoke_e2e.py
resolves the repo root via __file__.parents[3].
RUNNER_TEMP, not /tmp: see the Tiny AI job's venv step. TMPDIR
is redirected there too, because pip stages wheels in $TMPDIR.
test_server.py, test_smoke_e2e.py and test_score_extras_adr1117.py
score the src01 pair; without it they skipped (ADR-1528).
The MCP server's `_vmaf_binary()` defaults to
`<repo-root>/build/tools/vmaf` (`Path(__file__).parents[4]`
is the top-level repo root, not `core/`). The CI build
lives at `core/build-mcp/tools/vmaf`, so point
`VMAF_BIN` there explicitly so
test_call_tool_list_backends_includes_cpu's `_list_backends`
probe returns `cpu: True`.
===== Coverage gate (principles.md §3) =====
≥70% overall line coverage, ≥85% on security-critical files
(core/src/dnn/, opt.c, read_json_model.c).
```

## coverage

```text
75, not 30: the Python suite (four shard steps below, about 33 minutes
together on master 777647fc0) and the rest of the job (apt + pip, the
gcov build, the meson unit suite, gcovr; ~5.5 minutes) leave room for a
runner 1.5x slower than that measurement.
Sized as step caps (5+35+5+10 = 55) plus 1.25x measured 57m uncapped run (2026-10-23); 120 min covers sum.
Restore-only; the save below runs only after a successful install.
Caches downloaded .deb archives so a slow package mirror does not eat
into the apt step's timeout. Keyed on runner OS, arch, job, and the
hash of the job's apt package list under .github/apt/coverage.txt.
Measured max 30m21s slow fetch (2026-10-23); 35 min is at least 1.25x.
The hosted runner ships pre-provisioned Microsoft/Azure apt
sources (packages.microsoft.com) that intermittently 403 /
"no longer signed", failing `apt-get update` with exit 100
(observed on the Coverage Gate 2026-06-13). vmaf needs none of
them — drop them before updating. See T-CI-APT-MS-REPO-FLAKE.
rm -f succeeds when a pattern matches nothing.
meson from PyPI: Ubuntu 24.04 ships meson 1.3.2, which predates `c23`
in c_std (added in meson 1.4.0) and fails configure since ADR-0692.
gcovr replaces lcov: it deduplicates .gcno files belonging to
the same source compiled into multiple targets (libvmaf.so +
each test_X.p/.._src_X.c.gcda), which lcov sums and yields
impossible >100% hit counts (e.g. dnn_api.c — 1176%). gcovr
also produces both Cobertura XML for CI tooling and a JSON
summary the gate script parses directly. See ADR-0110.
setuptools>=77.0.1: the runner image ships 68.1.2 in
/usr/lib/python3/dist-packages and `pip install --user` leaves it
there, but python/pyproject.toml declares its license as a PEP 639
SPDX expression, which only setuptools 77+ parses. Without this the
ambient interpreter cannot run `python/setup.py` at all — which is
how setup_metadata_test.py failed the first time this lane ran the
whole Python suite to completion.
The editable install compiles vmaf.core.adm_dwt2_cy, as tox does
(python/tox.ini commands_pre); cy_test.py imports it. Without it
the suite ignored cy_test.py since #18 (ADR-1528).
Saves only after the install step succeeded: an install cut off by its
timeout leaves the archive directory empty, and an empty entry under
this immutable key would be restored as a hit on every later run
(an empty 200-byte entry was saved this way on 2026-10-22). Runs even
when a later step fails, so the next run skips the download.
Cache the ORT GPU .tgz so we don't re-download ~150 MB from
github.com/microsoft/onnxruntime/releases on every Coverage Gate
run. Key is purely the ORT version pinned in the env block of the
next step — the artifact bytes are immutable per release. See
Research-0089 (PR #525) §2 for the wall-clock evidence (Coverage
Gate p50 = 13.32 min, the ORT download contributes ~30–60 s of
that on a cold runner).
Measured max 1.3m (2026-10-23); 10 min cap covers tarball download and setup at 1.25x.
The GPU .tgz bundles CPU + CUDA + TensorRT execution providers in
one package. We install it (instead of the smaller CPU-only build)
so the coverage gate can also exercise the CUDA EP-attach success
arms in ort_backend.c — which on a CPU-only ORT remained
unreachable per ADR-0112. CUDA EP attach only requires the CUDA
*runtime* library to load (no actual GPU driver), which is why
libcudart12 is installed above. CreateSession then fails because
there's no real GPU on a hosted runner; vmaf_ort_open's two-stage
fallback (ADR-0113) catches that and retries with CPU-only
session_options, so existing tests stay green while the EP-attach
success arm and the new fallback path both execute.
CUDA 12.x runtime — user-space libs only, no driver. Lets ORT
dlopen libonnxruntime_providers_cuda.so → libcudart.so.12.
libcudart12 is cached in the apt step; fall back to nvidia-cuda-toolkit
only if libcudart12 was not installed.
Reuse cached tarball if present (cache hit), otherwise fetch
and populate the cache for the next run.
Build inside core/ so python/vmaf/__init__.py finds the CLI at
core/build-coverage/tools/vmaf (it hardcodes that relative path).
The suite's Go-running tests find their modules in the cache: the
download happens here, retried, and fails here when it cannot succeed
(scripts/ci/prefetch_go_modules.py, as the user the suite runs as).
--timeout-multiplier 3: the suite's timeouts are sized for an
uninstrumented build (docs/development/ci.md, "Sanitizer legs").
python/vmaf/__init__.py hardcodes core/build/tools/vmaf as the
CLI path. Symlink build-coverage → build so coverage runs find it.
The suite runs as four round-robin shards of one step each
(scripts/ci/run_pytest_shard.py): about 890 items took 33 minutes
in one step (master 777647fc0, job 117249828983), above praetor's
30-minute bound on a test step. gcov counters in the .gcda files
add up across the four processes, so gcovr below sees the whole
suite. Every shard keeps the unsharded command line:
- --deselect matches nothing and -m "not slow" selects everything:
  python/tox.ini makes python/ the rootdir, so node ids read
  test/command_line_test.py::…, and no test is marked slow
  (measured 2026-09-22, jobs 106740470243 and 106785621316). Both
  stay so the suite that runs is the suite that is measured.
- --timeout=600 with the thread method aborts the whole session
  when one test crosses it. It covers the slowest test measured
  (vifks360o97, 142.7 s) times the worst runner inflation seen
  (2.30x) with a 1.8x margin.
- The outer `timeout` only keeps a wedged vmaf subprocess from
  eating the job budget before gcovr runs; a shard took about
  9 minutes in the measured job.
gcovr deduplicates .gcno files belonging to the same source
compiled into multiple targets — fixes the lcov over-counting
that produced dnn_api.c — 1176%. Output:
  - coverage.xml (Cobertura, for downstream tooling)
  - coverage.json (machine-readable summary the gate parses)
  - coverage.txt (human-readable, surfaced in CI logs)
Suspicious/negative hit warnings on hot inner loops (e.g.
4.9 G hits on a 5x5 filter inner loop across
an HD frame batch — real, not a gcov bug) are filtered out so
the Coverage Gate stays warning-free. Underlying data is still
ingested via the .warn settings; only the chatty stderr line is
dropped. See ADR-0117.
docs/principles.md §3 targets: 70% overall, 85% critical (dnn/,
opt.c, read_json_model.c). Applies to upstream + fork-added code:
we've already linted upstream heavily (clang-tidy cleanup rounds
2-4) and will keep modifying it (SIMD fixes, refactors), so the
safety net has to cover everything we might touch.

Floor history (ADR-0637):
  40% — original floor (meson unit suite + Python feature/CLI tests)
  37% — 2026-05-19 merge burst (PRs #1417, #1418, #1424, #1425)
         added ~2 200 LOC of new MCP/HIP/DNN/scaffold C that the
         existing test suite does not yet reach, diluting overall
         coverage from ~39% to 37.7%. The floor tracks the measured
         floor, not the aspirational target; ratchet upward as
         targeted tests land for those paths.
A shard is skipped when a step before it failed; the error names
that step (run 39006126494: the meson suite timed out and the
shards only reported "skipped").
===== GPU coverage admission (hosted) =====
ADR-1319: the operator switch is not dispatch authority. Check the live
runner inventory on hosted compute first so a stale true variable cannot
leave a job queued forever on an impossible label set.
```

## gpu-full-runner-available

```text
Existing read-only repository runner token; no new secret or
external runner mutation is required by this repository fix.
===== GPU coverage (self-hosted) =====
Exercises fork-added CUDA / SYCL kernels, GPU runtime glue, and
AVX-512 SIMD paths that hosted runners can't touch. Runs on a self-hosted
runner labeled `gpu-full` (NVIDIA CUDA + Intel Arc SYCL +
AVX-512 CPU). See docs/development/self-hosted-runner.md for setup.

The two-week stability window closed on 2026-06-02. Keep this required;
the promotion was recorded in the changelog, but a later stale merge
restored the advisory workflow body.
```

## coverage-gpu

```text
Full history for the merge-base tests of the suite
(scripts/ci/tests/test_meson_suite_checkout_history.py).
icpx required for SYCL link — see core/src/meson.build.
See CPU coverage job for -fprofile-update=atomic rationale.
GPU build also runs SIMD CPU paths, so the same .gcda race applies.
b_lto=false: see libvmaf-build-matrix.yml note. Coverage build
uses --buildtype=debug which already disables LTO, but make it
explicit to avoid a future buildtype change re-introducing the
icx + -flto=auto incompatibility.
ADR-1360: coverage needs no AOT device images; the self-hosted
host has no ocloc, so build the SPIR-V JIT path only.
The suite's Go-running tests find their modules in the cache: the
download happens here, retried, and fails here when it cannot succeed
(scripts/ci/prefetch_go_modules.py, as the user the suite runs as).
See CPU coverage job for --num-processes 1 rationale: serialise
to keep multi-process .gcda merges race-free. --timeout-multiplier 3:
the suite's timeouts are sized for an uninstrumented build
(docs/development/ci.md, "Sanitizer legs").
setuptools>=77.0.1: the runner image ships 68.1.2 in
/usr/lib/python3/dist-packages and `pip install --user` leaves it
there, but python/pyproject.toml declares its license as a PEP 639
SPDX expression, which only setuptools 77+ parses. Without this the
ambient interpreter cannot run `python/setup.py` at all — which is
how setup_metadata_test.py failed the first time this lane ran the
whole Python suite to completion.
Scope GPU test runs with VMAF_FORCE_BACKEND away from the Netflix golden assertions
(ADR-0024 / AGENTS.md §8) to avoid failing on ULP-relaxed GPU float differences.
Same gcovr migration as CPU coverage job (see ADR-0110); same
suspicious-hits stderr filter as the CPU job (see ADR-0117).
===== Power of 10 rule 5 — assertion density =====
Every fork-added (non-trivial) function must contain ≥1 assert().
```
