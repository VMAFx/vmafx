# Lint / CI carve-out inventory (2026-09-02)

Evidence base for [ADR-1142](../adr/1142-whole-codebase-standards.md).
Produced by an
`agy` (Gemini) agent instructed to open every lint/CI configuration file and
cite
`file:line` for each scope restriction, then reviewed while writing the ADR.
Machine-readable source: session scratchpad `carveouts.json` (218 rows). Line
numbers refer to master `a17bb31f8`.

## Method

Searched: `.github/workflows/*.yml` (`paths` / `paths-ignore`, narrowing `if:`,
`continue-on-error`, advisory jobs, `|| true`, changed-files selection),
`.clang-tidy`, `.cppcheck-suppressions.txt`, `.semgrep.yml` / `.semgrepignore`,
`pyproject.toml`, `python/tox.ini`, `.pre-commit-config.yaml`, `Makefile` lint
targets, `scripts/ci/*`, `core/meson.build` warning flags,
`.github/codeql-config.yml`, plus in-source suppression counts per directory
(`NOLINT*`, `#pragma … diagnostic ignored`, `# noqa`, `# nosec`, `# type:
ignore`,
`# shellcheck disable`, `nosemgrep`, `//nolint`, `#[allow(`).

## Totals

| Kind | Rows |
| --- | --- |
| path-exclude | 89 |
| check-disabled | 54 |
| suppression-list | 32 |
| inline-suppression-count | 17 |
| advisory-job | 10 |
| threshold | 10 |
| changed-files-only | 6 |

| Recommended action | Rows |
| --- | --- |
| REMOVE | 129 |
| KEEP-WITH-REASON | 62 |
| NEEDS-TOOLCHAIN | 27 |

## REMOVE (112 rows)

| Where | What it exempts | Stated reason |
| --- | --- | --- |
| `.github/workflows/build.yml:32` | Workflow execution on documentation and fragment changes (paths-ignore: docs/\*\*, \*\*/\*.md, changelog.d/\*\*, CHANGELOG.md, | ADR-0341: skip CI runs for doc-only changes |
| `.github/workflows/docker-image.yml:5` | Docker image build on push for changes outside core/\*\*, Dockerfile, .dockerignore, meson build files | ADR-0317: Only fire when Docker build inputs actually change. Doc-only / Python-only PRs d |
| `.github/workflows/docker-image.yml:24` | Docker image build on PR for changes outside core/\*\*, Dockerfile, .dockerignore, meson build files | ADR-0317 / ADR-0331: Path filter on pull request |
| `.github/workflows/ffmpeg-integration.yml:5` | FFmpeg integration matrix on push for changes outside core/\*\*, ffmpeg-patches/\*\*, meson build files | ADR-0317: Only fire when inputs it consumes change; doc-only/Python-only PRs were burning |
| `.github/workflows/ffmpeg-integration.yml:23` | FFmpeg integration matrix on PR for changes outside core/\*\*, ffmpeg-patches/\*\*, meson build files | ADR-0317 / ADR-0331: Path filter on pull request |
| `.github/workflows/go-ci.yml:5` | Go CI workflow on push for changes outside \*\*/\*.go, go.mod, go.sum | none (implicit path filtering for Go subtree) |
| `.github/workflows/go-ci.yml:10` | Go CI workflow on PR for changes outside \*\*/\*.go, go.mod, go.sum | none (implicit path filtering for Go subtree) |
| `.github/workflows/helm-chart.yml:11` | Helm chart workflow on push for changes outside deploy/helm/\*\* | none (implicit path filtering for Helm chart) |
| `.github/workflows/helm-chart.yml:15` | Helm chart workflow on PR for changes outside deploy/helm/\*\* | none (implicit path filtering for Helm chart) |
| `.github/workflows/libvmaf-build-matrix.yml:20` | Build matrix on PR for doc changes (paths-ignore: docs/\*\*, \*\*/\*.md, changelog.d/\*\*, CHANGELOG.md, .workingdir2/\*\*) | ADR-0341 / Research-0089 §3.2: skip full 18-cell build matrix when only documentation path |
| `.github/workflows/libvmaf-build-matrix.yml:62` | macOS clang (CPU) build leg failure gating (experimental: true) | macOS leg stays experimental: true so a Homebrew bump doesn't block merge |
| `.github/workflows/libvmaf-build-matrix.yml:109` | macOS clang (CPU) + DNN build leg failure gating (experimental: true) | macOS leg stays experimental: true so a Homebrew ORT bump that breaks ABI doesn't block me |
| `.github/workflows/libvmaf-build-matrix.yml:208` | Matrix build job failure gating (continue-on-error: ${{ matrix.experimental == true }}) | ADR-0726: continue-on-error now depends only on matrix.experimental |
| `.github/workflows/lint-and-format.yml:78` | Clang-Tidy C/C++ early skip probe based on git diff | Early file-delta probe — if PR/push touches no C/C++ files, short-circuit before apt-insta |
| `.github/workflows/lint-and-format.yml:198` | Clang-Tidy excludes core/test/tiny_ai_test_template.h | Template header evaluated only in instantiated test contexts |
| `.github/workflows/lint-and-format.yml:203` | Clang-Tidy excludes core/src/mcp/ | requires -Denable_mcp=true meson setup |
| `.github/workflows/lint-and-format.yml:204` | Clang-Tidy excludes core/test/test_mcp\* | requires -Denable_mcp=true meson setup |
| `.github/workflows/lint-and-format.yml:208` | Clang-Tidy excludes core/test/fuzz/ | requires -Dfuzz=true and libFuzzer instrumentation |
| `.github/workflows/lint-and-format.yml:209` | Clang-Tidy excludes core/src/compat/win32/ | Win32 pthread / getopt compatibility shims |
| `.github/workflows/lint-and-format.yml:344` | Clang-Tidy SYCL job changed-files detection (git diff --name-only) | Detect changed SYCL files; skips job body if empty |
| `.github/workflows/lint-and-format.yml:509` | mypy check on ai/ and scripts/ (// echo 'mypy advisory only on first run') | pre-commit hook runs it; advisory exit because numpy/torch stubs |
| `.github/workflows/lint-and-format.yml:633` | Markdownlint job changed-files detection (git diff --name-only) | ADR-0866: only staged/changed \*.md files are linted so pre-existing warning tail does not |
| `.github/workflows/nightly.yml:64` | Nightly clang-tidy full run swallows errors (// true) | Nightly diagnostic sweep; produces full log without failing |
| `.github/workflows/nightly.yml:85` | Nightly benchmark run swallows errors (bash testdata/bench_all.sh // true) | Swallows benchmark exit code |
| `.github/workflows/rule-enforcement.yml:120` | Deep-dive deliverables gate scopes to changed files (git diff --name-only ${BASE_SHA}..${HEAD_SHA}) | ADR-0108 deep dive deliverables check scoped to PR file delta |
| `.github/workflows/rule-enforcement.yml:270` | ADR-Backfill Advisory job (continue-on-error: true) | ADR-0106 advisory: policy / public-surface paths changed without a new ADR |
| `.github/workflows/rule-enforcement.yml:290` | ADR-Backfill Advisory scopes to changed files (git diff --name-only ${BASE_SHA}..${HEAD_SHA}) | Scopes ADR detection to changed files in PR |
| `.github/workflows/rust-ci.yml:6` | Rust CI workflow on push for changes outside bindings/rust/\*\*, core/src/feature/rust/\*\*, Cargo.\*, deny.toml | none (scoped to Rust files) |
| `.github/workflows/rust-ci.yml:14` | Rust CI workflow on PR for changes outside bindings/rust/\*\*, core/src/feature/rust/\*\*, Cargo.\*, deny.toml | none (scoped to Rust files) |
| `.github/workflows/sanitizers.yml:27` | Sanitizers workflow on PR for doc changes (paths-ignore: docs/\*\*, \*\*/\*.md, changelog.d/\*\*, CHANGELOG.md, .workingdir2/\*\* | Skip sanitizer runs on doc-only changes |
| `.github/workflows/sanitizers.yml:146` | ASan/UBSan deselects 5 test targets (grep -vE 'test_model$/test_y4m_alloc_failure$/test_gpu_picture_pool_uaf$/test_integ | Tracked as T-GPU-POOL-UAF-OOM-TSAN-ABORT... ADR-0347 deselect list; allocators fatal on in |
| `.github/workflows/sanitizers.yml:235` | TSan deselects 5 test targets (grep -vE 'test_model$/test_y4m_alloc_failure$/test_gpu_picture_pool_uaf$/test_integer_mot | TSan deselect list (ADR-0347 / T-GPU-POOL-UAF-OOM-TSAN-ABORT) |
| `.github/workflows/security-scans.yml:60` | Semgrep registry rule packs step (continue-on-error: true) | We don't gate merges on registry findings; CodeQL is merge gate... continue-on-error becau |
| `.github/workflows/security-scans.yml:81` | Semgrep registry rule packs scan swallows errors (--sarif --output=semgrep-registry.sarif // true) | Swallows exit code from registry rule packs scan |
| `.github/workflows/tests-and-quality-gates.yml:25` | Quality gates workflow on PR for doc changes (paths-ignore: docs/\*\*, \*\*/\*.md, changelog.d/\*\*, CHANGELOG.md, .workingdir2 | Skip quality gates on doc-only changes |
| `.github/workflows/tests-and-quality-gates.yml:253` | Sanitizers matrix deselects test_model under address sanitizer (EXCLUDE='test_model$') | ASan deselects: test_model (svm.cpp parser alloc-too-big on malformed JSON)... ADR-0347 |
| `.github/workflows/tests-and-quality-gates.yml:265` | Sanitizers matrix deselects test_model under undefined sanitizer (EXCLUDE='test_model$') | UBSan deselects: test_model (NULL-to-nonnull memcpy in svm.cpp parser)... ADR-0347 |
| `.github/workflows/tests-and-quality-gates.yml:274` | Sanitizers matrix deselects test_model under thread sanitizer (EXCLUDE='test_model$') | TSan deselects: test_model (same defect as ASan/UBSan)... ADR-0347 |
| `.github/workflows/tests-and-quality-gates.yml:637` | CPU coverage pytest excludes python/test/resource, cy_test.py, cambi_test.py | Resource files are non-importable configs; cy_test and cambi_test excluded from coverage r |
| `.github/workflows/tests-and-quality-gates.yml:640` | CPU coverage pytest deselects CommandLineTest and swallows exit code (--deselect python/test/command_line_test.py::Comma | Deselecting CommandLineTest lets alphabetically-later suites execute and contribute covera |
| `.github/workflows/e2e-k8s.yml:87` | Kubernetes e2e test suite step has continue-on-error: true | none |
| `.clang-tidy:22` | Disabled clang-tidy check: -bugprone-easily-swappable-parameters | none (introduced in commit 7af089949 without comment) |
| `.clang-tidy:23` | Disabled clang-tidy check: -bugprone-narrowing-conversions | none (introduced in commit 7af089949 without comment) |
| `.clang-tidy:24` | Disabled clang-tidy check: -clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling | none (introduced in commit 7af089949 without comment) |
| `.clang-tidy:25` | Disabled clang-tidy check: -misc-include-cleaner | none (introduced in commit 7af089949 without comment) |
| `.clang-tidy:26` | Disabled clang-tidy check: -misc-no-recursion | none (introduced in commit 7af089949 without comment) |
| `.clang-tidy:27` | Disabled clang-tidy check: -modernize-use-trailing-return-type | none (introduced in commit 7af089949 without comment) |
| `.clang-tidy:28` | Disabled clang-tidy check: -modernize-use-auto | none (introduced in commit 7af089949 without comment) |
| `.clang-tidy:29` | Disabled clang-tidy check: -modernize-avoid-c-arrays | none (introduced in commit 7af089949 without comment) |
| `.clang-tidy:30` | Disabled clang-tidy check: -modernize-use-nodiscard | none (introduced in commit 7af089949 without comment) |
| `.clang-tidy:31` | Disabled clang-tidy check: -modernize-avoid-c-style-cast | none (introduced in commit 7af089949 without comment) |
| `.clang-tidy:32` | Disabled clang-tidy check: -modernize-macro-to-enum | none (introduced in commit 7af089949 without comment) |
| `.clang-tidy:33` | Disabled clang-tidy check: -modernize-avoid-variadic-functions | none (introduced in commit 7af089949 without comment) |
| `.clang-tidy:35` | WarningsAsErrors subset: only 10 specific checks treated as fatal errors | none (progressive adoption gating in commit 7af089949) |
| `.clang-tidy:47` | HeaderFilterRegex: '^(core/(include/src/tools/test)/python/ai)/.\*\.(h/hpp/hxx/cuh)$' | Lint our code; skip vendored / generated. |
| `.cppcheck-suppressions.txt:7` | Cppcheck excludes \*:core/src/svm.cpp | Upstream Netflix svm.cpp — entirely vendored, not modified by fork. |
| `.cppcheck-suppressions.txt:21` | Cppcheck suppresses unusedFunction on core/src/feature/\*_avx512.c | False positives we've reviewed; revisit yearly. |
| `.cppcheck-suppressions.txt:22` | Cppcheck suppresses unusedFunction on core/src/feature/\*_avx2.c | False positives we've reviewed; revisit yearly. |
| `.cppcheck-suppressions.txt:23` | Cppcheck suppresses unusedFunction on core/src/feature/\*_neon.c | False positives we've reviewed; revisit yearly. |
| `.cppcheck-suppressions.txt:24` | Cppcheck suppresses unusedFunction on core/src/cuda/\*.cu | False positives we've reviewed; revisit yearly. |
| `.cppcheck-suppressions.txt:25` | Cppcheck suppresses unusedFunction on core/src/sycl/\*.cpp | False positives we've reviewed; revisit yearly. |
| `.cppcheck-suppressions.txt:31` | Cppcheck suppresses uninitvar on core/test/test_propagate_metadata.c | cppcheck 2.13 doesn't recognise = {0} aggregate init as covering all members. |
| `.cppcheck-suppressions.txt:32` | Cppcheck suppresses uninitStructMember on core/test/test_propagate_metadata.c | cppcheck 2.13 doesn't recognise = {0} aggregate init as covering all members. |
| `.cppcheck-suppressions.txt:36` | Cppcheck suppresses invalidPrintfArgType_sint on core/src/output.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:37` | Cppcheck suppresses invalidPrintfArgType_sint on core/src/read_json_model.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:38` | Cppcheck suppresses invalidPrintfArgType_sint on core/test/test_framesync.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:39` | Cppcheck suppresses invalidPrintfArgType_sint on core/test/test_predict.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:40` | Cppcheck suppresses invalidPrintfArgType_sint on core/test/test_thread_pool.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:41` | Cppcheck suppresses invalidPrintfArgType_sint on core/tools/vmaf.cpp | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:42` | Cppcheck suppresses uninitvar on core/src/predict.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:43` | Cppcheck suppresses arithOperationsOnVoidPointer on core/src/feature/integer_vif.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:44` | Cppcheck suppresses invalidPointerCast on core/src/opt.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:45` | Cppcheck suppresses invalidPointerCast on core/src/opt.cpp | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:46` | Cppcheck suppresses invalidPointerCast on core/src/feature/x86/float_adm_avx2.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:47` | Cppcheck suppresses duplicateAssignExpression on core/src/feature/integer_adm.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:48` | Cppcheck suppresses duplicateAssignExpression on core/src/feature/x86/adm_avx2.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:49` | Cppcheck suppresses duplicateAssignExpression on core/src/feature/x86/adm_avx512.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:50` | Cppcheck suppresses shiftNegativeLHS on core/src/feature/x86/adm_avx2.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:51` | Cppcheck suppresses shiftNegativeLHS on core/src/feature/x86/adm_avx512.c | Upstream Netflix code: printf format / pointer cast / void arithmetic warnings. These are |
| `.cppcheck-suppressions.txt:53` | Cppcheck suppresses missingIncludeSystem globally | none |
| `.cppcheck-suppressions.txt:54` | Cppcheck suppresses unmatchedSuppression globally | none |
| `.semgrep.yml:57` | vmaf-no-system-rand rule excludes /core/src/svm.cpp | Upstream Netflix SVM cross-validation uses rand() for fold shuffling. Tracked as cleanup d |
| `.semgrep.yml:71` | vmaf-no-strcpy-strcat-sprintf rule excludes /matlab/\*\*, /core/tools/cli_parse.c, /core/tools/y4m_input.c | Upstream Netflix tree — tracked as cleanup debt, not gated here: cli_parse.c (sprintf), y4 |
| `.semgrepignore:10` | Semgrep ignores python/vmaf/matlab/ and compat/python-vmaf/matlab/ | Netflix-upstream MATLAB MEX helpers. Pure research reference, not linked into libvmaf... ( |
| `pyproject.toml:35` | Black extend-exclude: .git, build, dist, subprojects, python/test/resource, compat/python-vmaf/resource, compat/python-v | none |
| `pyproject.toml:51` | Ruff extend-exclude: build, subprojects, python/test/resource, compat/python-vmaf/resource, compat/python-vmaf/matlab | none |
| `pyproject.toml:75` | Ruff ignore list: E501, PLR0913, S101, PLR0917, BLE001, S110, TRY004, SIM102, SIM103, SIM117, PLC0206, PLR0124, PLW1510, | Rules that began firing with ruff 0.15.17 -> 0.16.5 bump... deferred rather than silenced. |
| `pyproject.toml:116` | Ruff per-file-ignores on 'python/\*\*' (S, PL, B, C4, PIE, RET, SIM, RUF, I001, PTH, F401, F522, F541, F841, E402, E711, E | Upstream Netflix Python is not our style canon. Keep F821... silence bugbear / security / |
| `pyproject.toml:125` | Ruff per-file-ignores on 'testdata/\*\*' (S, PL, B, SIM, RUF, I001, PTH, F401, F541, F841, E401, E402, E701, E722, W291, W | Ad-hoc developer benchmark + one-off comparison scripts... style drift is acceptable and b |
| `pyproject.toml:135` | Ruff per-file-ignores on 'compat/python-vmaf/\*\*' (S, PL, B, C4, PIE, RET, SIM, RUF, I001, PTH, F401, F522, F541, F841, E | Upstream Netflix Python harness, relocated from python/vmaf/ to compat/python-vmaf/ by VMA |
| `pyproject.toml:145` | Ruff per-file-ignores on 'compat/python-vmaf/resource/\*\*' (S, PL, B, SIM, RUF, I001, PTH, F401, F541, F841, E401, E402, | Upstream Netflix dataset / model config scripts. Pure data, not business logic... belt-and |
| `pyproject.toml:173` | Mypy excludes build/, subprojects/, python/test/resource/, ai/src/ | ai/src/ itself is excluded from file-scan list to prevent duplicate-module-name error |
| `pyproject.toml:180` | Mypy override ignores errors on 'vmaf.\*' (ignore_errors = true) | upstream library — not our type stubs to maintain |
| `pyproject.toml:184` | Mypy override ignores errors on 'ai.src.\*', 'ai.src.vmaf_train.\*' (ignore_errors = true) | Suppress 'Source file found twice' errors for ai.src.vmaf_train.\* namespace |
| `python/tox.ini:26` | Tox pytest disables warnings (-p no:warnings) | TODO: way too many warnings in tests, remove `-p no:warnings` and get rid of them all |
| `python/tox.ini:42` | Tox coverage env ignores non-zero exit code (ignore_outcome = true) | ignore_outcome = true prevents a non-zero exit from this env from failing overall tox invo |
| `.pre-commit-config.yaml:75` | black hook scoped to (python/ai/scripts/tools) and excludes python/test/resource, compat/python-vmaf/resource, matlab | Upstream Netflix training-harness tree — not authored by us; see pyproject.toml [tool.blac |
| `.pre-commit-config.yaml:83` | ruff-check hook scoped to (python/ai/scripts/tools) and excludes python/test/resource, compat/python-vmaf/resource, matl | Upstream Netflix training-harness tree — not authored by us |
| `.pre-commit-config.yaml:227` | assertion-density hook passes pass_filenames: false | The script self-discovers fork-added files via the 'Copyright \* Lusoris' copyright marker; |
| `.pre-commit-config.yaml:236` | mypy-local hook scoped to '^(ai/scripts)/.\*\.py$' | Pre-push only — running mypy on every commit doubles a 1-line edit's commit time |
| `.pre-commit-config.yaml:288` | semgrep-local hook excludes subprojects, test/data, resource, matlab, pdjson.{c,h} | Mirror check-copyright's exclude list — same upstream / vendored paths that ship with thei |
| `Makefile:155` | lint-c: clang-tidy excludes subprojects and core/src/interop/pelorus_\*; omits core/test, GPU kernels, compat | FILES=$(git ls-files 'core/src/\*\*/\*.c' 'core/src/\*\*/\*.cpp' 'core/tools/\*.c' / grep -v '^su |
| `Makefile:160` | lint-c: cppcheck uses --suppressions-list=.cppcheck-suppressions.txt | Uses suppression file |
| `Makefile:167` | lint-py: ruff check python/ ai/ scripts/ & black --check python/ ai/ scripts/ (omits tools/, compat/, testdata/) | Scoped to selected Python directories |
| `Makefile:176` | lint-py: -mypy ai/scripts/ ai/tests/ ai/train/ ai/lpips_export.py scripts/ (leading '-' ignores failures) | mypy is advisory (leading -): it currently reports ~295 module-resolution errors... that s |
| `Makefile:190` | lint-md: MDLINT_SCOPE ?= changed defaults to changed files vs origin/master | ADR-0866: Default scope is the touched-file delta vs origin/master so the ~6.2k pre-existi |
| `Makefile:219` | format: format commands append '// true' and exclude subprojects, test/data, pelorus | Swallows formatter exit codes |
| `scripts/ci/assertion-density.sh:29` | assertion-density.sh excludes upstream Netflix files and Pelorus mirror | Scope: files whose copyright header contains a Lusoris copyright line... Upstream Netflix |
| `core/meson.build:8` | default_options warning_level=2 (werror=false) | Meson default options |
| `core/src/meson.build:1886` | libsvm_cpp_args += ['-U_LIBCPP_ENABLE_ASSERTIONS'] on clang/apple-clang | Undefines libc++ assertions on clang/apple-clang for vendored libsvm |
| `.github/codeql-config.yml:26` | CodeQL paths whitelist restricts analysis to 10 specific directory trees | Limits CodeQL scanning scope |
| `.github/codeql-config.yml:38` | CodeQL paths-ignore excludes subprojects, build, core/test, python/test, testdata, third_party, generated, gen/go | Excludes test suites, build artifacts, fixtures, generated code |

## NEEDS-TOOLCHAIN (27 rows)

| Where | What it exempts | Stated reason |
| --- | --- | --- |
| `.github/workflows/libvmaf-build-matrix.yml:532` | Meson unit test execution on sycl, cuda, hip, i686 (if: !matrix.sycl && !matrix.cuda && !matrix.hip && !matrix.i686) | ADR-0151 (i686 cross-build marks tests SKIP 77); GPU backends require hardware not on GitH |
| `.github/workflows/libvmaf-build-matrix.yml:575` | Python test suite on sycl, cuda, hip, i686 (if: startsWith(matrix.os, 'ubuntu') && !matrix.sycl && !matrix.cuda && !matr | GPU test execution requires physical GPU silicon |
| `.github/workflows/lint-and-format.yml:193` | Clang-Tidy excludes core/src/feature/arm64/ | compile commands in build/compile_commands.json don't cover NEON, needs cross-toolchain |
| `.github/workflows/lint-and-format.yml:194` | Clang-Tidy excludes core/src/cuda/ | requires -Denable_cuda=true meson setup to generate compile_commands (backlog T7-3) |
| `.github/workflows/lint-and-format.yml:195` | Clang-Tidy excludes core/src/feature/cuda/ | requires -Denable_cuda=true meson setup to generate compile_commands |
| `.github/workflows/lint-and-format.yml:196` | Clang-Tidy excludes core/test/test_cuda_\* | GPU-gated tests whose build entries only exist under CUDA build |
| `.github/workflows/lint-and-format.yml:197` | Clang-Tidy excludes core/test/test_gpu_picture_pool.c | ADR-0239: CUDA-only test pulling in libvmaf_cuda.h |
| `.github/workflows/lint-and-format.yml:199` | Clang-Tidy excludes core/src/sycl/ | requires -Denable_sycl=true and icpx compiler |
| `.github/workflows/lint-and-format.yml:200` | Clang-Tidy excludes core/src/feature/sycl/ | requires -Denable_sycl=true and icpx compiler |
| `.github/workflows/lint-and-format.yml:201` | Clang-Tidy excludes core/test/test_sycl\* | requires -Denable_sycl=true and icpx compiler |
| `.github/workflows/lint-and-format.yml:202` | Clang-Tidy excludes core/tools/vmaf_vpl.c | VAAPI integration; needs va/va.h from libva-dev plus CUDA/SYCL link path |
| `.github/workflows/lint-and-format.yml:205` | Clang-Tidy excludes core/src/hip/ | requires HIP ROCm toolchain |
| `.github/workflows/lint-and-format.yml:206` | Clang-Tidy excludes core/src/feature/hip/ | requires HIP ROCm toolchain |
| `.github/workflows/lint-and-format.yml:207` | Clang-Tidy excludes core/test/test_hip\* | requires HIP ROCm toolchain |
| `.github/workflows/lint-and-format.yml:323` | Clang-Tidy SYCL job (continue-on-error: true) | Advisory (continue-on-error: true) until one green master run confirms |
| `.github/workflows/tests-and-quality-gates.yml:126` | Cross-backend ULP Diff CPU sanity job disabled (if: false) | Requires GPU hardware + local YUV fixtures not present on GitHub-hosted runners. Disabled |
| `.github/workflows/tests-and-quality-gates.yml:709` | Coverage Gate — GPU Backends marked advisory (continue-on-error: true) | Advisory-only (continue-on-error) for now: merges gated on CPU coverage... Will be promote |
| `.github/workflows/tests-and-quality-gates.yml:755` | GPU coverage CUDA pytest run swallows errors (VMAF_FORCE_BACKEND=cuda ... // true) | Swallows pytest failure on CUDA backend |
| `.github/workflows/tests-and-quality-gates.yml:758` | GPU coverage SYCL pytest run swallows errors (VMAF_FORCE_BACKEND=sycl ... // true) | Swallows pytest failure on SYCL backend |
| `Makefile:270` | test-sanitizers: disables CUDA and SYCL (-Denable_cuda=false -Denable_sycl=false) | Sanitizer build runs CPU-only |
| `Makefile:286` | coverage: disables CUDA and SYCL (-Denable_cuda=false -Denable_sycl=false) | Coverage build runs CPU-only |
| `scripts/ci/clang-tidy-sycl.sh:88` | clang-tidy-sycl.sh injects -D__SYCL_DEVICE_ONLY__=0, -Wno-unknown-warning-option, -Wno-unknown-pragmas, -std=c++17 | ADR-0217: skip device-only branches that require icpx device compiler... suppress kernel-i |
| `core/meson_options.txt:39` | option('enable_cuda', value: false) | Enable CUDA support (default false) |
| `core/meson_options.txt:54` | option('enable_sycl', value: false) | Enable SYCL/DPC++ support (default false) |
| `core/meson_options.txt:98` | option('enable_hip', value: false) | Build HIP (AMD ROCm) compute backend (default false) |
| `core/meson_options.txt:103` | option('enable_hipcc', value: false) | Compile HIP device kernels via hipcc (default false) |
| `core/meson_options.txt:118` | option('enable_metal', value: 'auto') | Build Metal (Apple Silicon) compute backend (default auto) |

## KEEP-WITH-REASON (62 rows)

| Where | What it exempts | Stated reason |
| --- | --- | --- |
| `.github/workflows/lint-and-format.yml:210` | Clang-Tidy excludes core/src/interop/pelorus_\* | ADR-1113: verbatim mirror of VMAFx/pelorus@835e097, byte-identical to upstream |
| `.github/workflows/lint-and-format.yml:211` | Clang-Tidy excludes core/include/libvmaf/pelorus/ | ADR-1113: Pelorus interop public header mirror |
| `.github/workflows/lint-and-format.yml:273` | Clang-Tidy excludes subprojects/ | Vendored upstream code in subprojects |
| `.github/workflows/lint-and-format.yml:651` | Markdownlint excludes subprojects/, test/data, resources, matlab, model, testdata | ADR-0866: Exclude upstream / vendored / fixture trees that ship markdown we do not author |
| `.github/workflows/lint-and-format.yml:652` | Markdownlint excludes docs/adr/README.md, CHANGELOG.md, docs/adr/_index_fragments/, changelog.d/ | ADR-0221: per-PR fragments that are concatenated mechanically... rendered output is what r |
| `.github/workflows/tests-and-quality-gates.yml:658` | CPU coverage gcovr excludes .\*/test/.\*, .\*/tests/.\*, .\*/subprojects/.\* | Exclude test code and subprojects from production library coverage calculation |
| `.github/workflows/tests-and-quality-gates.yml:668` | CPU coverage gcovr filters stderr warnings (2> >(grep -vE 'Ignoring (suspicious/negative) hits' >&2 // true)) | Suspicious/negative hit warnings on hot inner loops filtered out... ADR-0117 |
| `.github/workflows/tests-and-quality-gates.yml:767` | GPU coverage gcovr excludes .\*/test/.\*, .\*/tests/.\*, .\*/subprojects/.\* | Exclude test code and subprojects from GPU coverage gate |
| `.clang-tidy:7` | Initial minus-list (-\*) disables all default clang-tidy checks | See docs/principles.md §2 for philosophy... enable families that map directly to NASA Powe |
| `.clang-tidy:59` | readability-function-size thresholds (Line: 60, Statement: 120, Branch: 15, Nesting: 4) | Power-of-10 rule 4 — a printed page. Upstream Netflix functions that exceed this budget ar |
| `.cppcheck-suppressions.txt:2` | Cppcheck excludes \*:subprojects/\* | Vendored upstream code we don't own. |
| `.cppcheck-suppressions.txt:3` | Cppcheck excludes \*:core/test/data/\* | Vendored upstream code we don't own. |
| `.cppcheck-suppressions.txt:4` | Cppcheck excludes \*:build/\* | Vendored upstream code we don't own. |
| `.cppcheck-suppressions.txt:13` | Cppcheck excludes \*:core/src/interop/pelorus_interop.c | Vendored Pelorus interop ABI — verbatim mirror of VMAFx/pelorus@835e097 (ADR-1113) |
| `.cppcheck-suppressions.txt:14` | Cppcheck excludes \*:core/src/interop/pelorus_deband_params.c | Vendored Pelorus interop ABI — verbatim mirror of VMAFx/pelorus@835e097 (ADR-1113) |
| `.cppcheck-suppressions.txt:15` | Cppcheck excludes \*:core/src/interop/pelorus_version.c | Vendored Pelorus interop ABI — verbatim mirror of VMAFx/pelorus@835e097 (ADR-1113) |
| `.cppcheck-suppressions.txt:16` | Cppcheck excludes \*:core/include/libvmaf/pelorus/interop.h | Vendored Pelorus interop ABI — verbatim mirror of VMAFx/pelorus@835e097 (ADR-1113) |
| `.cppcheck-suppressions.txt:17` | Cppcheck excludes \*:core/include/libvmaf/pelorus/pelorus.h | Vendored Pelorus interop ABI — verbatim mirror of VMAFx/pelorus@835e097 (ADR-1113) |
| `.cppcheck-suppressions.txt:18` | Cppcheck excludes \*:core/include/libvmaf/pelorus/deband.h | Vendored Pelorus interop ABI — verbatim mirror of VMAFx/pelorus@835e097 (ADR-1113) |
| `.cppcheck-suppressions.txt:28` | Cppcheck suppresses unusedFunction on core/include/libvmaf/\*.h | Public C API surface — naturally has functions only called by external linkers. |
| `.semgrep.yml:41` | vmaf-no-printf-precision-below-17g rule restricted to output.c, libvmaf.c, vmaf.cpp | Only flag printf-family calls where score/metric variable is direct argument... A bare reg |
| `.semgrep.yml:79` | vmaf-no-strcpy-strcat-sprintf rule excludes /core/src/mcp/3rdparty/\*\* | Vendored third-party code — kept verbatim under original license... cJSON v1.7.18 (MIT) (A |
| `.semgrep.yml:110` | vmaf-no-skip-hooks-in-scripts rule restricted to \*\*/\*.sh, /.claude/\*\* | Hooks may not be skipped from scripts |
| `.semgrepignore:17` | Semgrep ignores python/vmaf/resource/ and compat/python-vmaf/resource/ | Netflix-upstream Python training resource tree (datasets, param/feature dicts)... (ADR-070 |
| `.semgrepignore:21` | Semgrep ignores subprojects/, build/, dist/, core/test/data/, model/, testdata/ | Standard exclusions — third-party / generated. |
| `.semgrepignore:32` | Semgrep ignores scripts/git-hooks/pre-push-mkdocs-strict.sh | is itself a hook; it documents the SKIP= / --no-verify bypass patterns as user-facing help |
| `.semgrepignore:39` | Semgrep ignores scripts/githooks/pre-commit.sh | contains a comment explaining the --no-verify escape hatch for users; it is documentation |
| `.semgrepignore:46` | Semgrep ignores core/src/mcp/3rdparty/cJSON/cJSON.c | Vendored upstream cJSON — three upstream TODO/FIXME markers... tracked in cJSON project it |
| `.semgrepignore:49` | Semgrep ignores binary / data extensions (\*.pkl, \*.mex\*, \*.onnx, \*.bin, \*.yuv, \*.npy) | Binary / data suffixes. |
| `.markdownlint.json:4` | MD013 (line length 80) disables checking tables and code blocks | none |
| `.markdownlint.json:8` | MD024 (duplicate headings) permits duplicate headings under different parent headings (siblings_only: true) | none |
| `pyproject.toml:191` | Mypy override ignore_missing_imports on 9 third-party packages (onnxruntime, lpips, openvino, jsonschema, sklearn, tenso | Packages that ship no py.typed marker and have no usable third-party stubs. Call sites are |
| `python/tox.ini:26` | Tox pytest excludes vmaf/resource (--ignore=vmaf/resource) | --ignore=vmaf/resource: pytest --doctest-modules tries to import every .py under rootdir.. |
| `.pre-commit-config.yaml:24` | trailing-whitespace excludes patches, subprojects, test/data, model, resources, matlab, testdata, binary extensions | upstream Netflix tree ships MATLAB sources, Python pickles (binary), and precompiled MEX.. |
| `.pre-commit-config.yaml:26` | end-of-file-fixer excludes patches, subprojects, test/data, model, resources, matlab, testdata, binary extensions | Same binary-file exclusions as trailing-whitespace / end-of-file-fixer above |
| `.pre-commit-config.yaml:36` | check-yaml excludes .clang-format, mkdocs.yml, deploy/helm/.\*/templates/, test/e2e/kuttl-tests/ | mkdocs.yml uses !!python/name tags... Helm templates contain Go template syntax... kuttl t |
| `.pre-commit-config.yaml:46` | mixed-line-ending excludes patches, subprojects, test/data, model, resources, matlab, testdata, binary extensions | mixed-line-ending's built-in binary detection misfires on some ONNX / .bin payloads and re |
| `.pre-commit-config.yaml:67` | clang-format excludes subprojects/, core/test/data/, core/src/interop/, core/include/libvmaf/pelorus/ | core/src/interop + core/include/libvmaf/pelorus = vendored read-only mirror of VMAFx/pelor |
| `.pre-commit-config.yaml:140` | markdownlint-cli2 excludes subprojects, test/data, resource, matlab, model, testdata, README.md, CHANGELOG.md, fragments | ADR-0866: Exclude upstream / vendored / fixture trees that ship markdown we do not author. |
| `.pre-commit-config.yaml:198` | check-copyright excludes subprojects, test/data, resource, matlab, pdjson.{c,h} | ADR-0105: Exclusions: (a) upstream vendored subprojects, (b) test fixtures, (c) resource d |
| `.pre-commit-config.yaml:211` | check-adr-numbering hook restricted to '^docs/adr/[0-9]{4}-.\*\.md$' | ADR-0386: Only fires when a docs/adr/NNNN-\*.md file is staged |
| `.pre-commit-config.yaml:301` | ffmpeg-patches-apply-check hook scoped to '^ffmpeg-patches/.\*\.patch$' | Script self-discovers via series.txt; passing per-file paths would re-check each patch onc |
| `Makefile:133` | lint-go: gosec -exclude-generated -quiet ./... | Skips generated files by default; surfaces every G\* finding outside the gen/ tree |
| `Makefile:199` | lint-md: excludes !docs/adr/README.md, !docs/adr/_index_fragments/\*\*, CHANGELOG.md, changelog.d/ | ADR-0221: Pipeline input fragments / autogenerated docs |
| `Makefile:229` | format-check: grep -v excludes subprojects, test/data, pelorus | Excludes vendored subprojects and Pelorus mirror |
| `Makefile:294` | coverage: lcov --remove removes '/usr/\*', '\*/subprojects/\*', '\*/test/\*', '\*/tests/\*' | Removes system headers, subprojects, and test code from coverage report |
| `Makefile:308` | Coverage thresholds: COVERAGE_MIN_OVERALL := 70, COVERAGE_MIN_CRITICAL := 85 | Enforce the coverage thresholds from docs/principles.md §3 |
| `scripts/ci/assertion-density.sh:17` | MIN_LINES=20 threshold for assertion density check | Policy: every fork-added C function ≥MIN_LINES lines (default 20) must contain ≥1 assert() |
| `scripts/ci/check-copyright.sh:34` | check-copyright.sh skips \*config.h.in / \*generated\* | Skip auto-generated files (e.g. meson's config.h.in, bison/flex output) that never carry a |
| `scripts/ci/coverage-check.sh:24` | OVERALL_MIN=70, CRITICAL_MIN=90 baseline coverage thresholds | docs/principles.md §3 targets: 70% overall, 85% critical; ADR-0922 ratchets critical floor |
| `scripts/ci/coverage-check.sh:32` | PER_FILE_MIN override for core/src/dnn/ort_backend.c = 83% | ADR-0114 baseline 78%; ADR-0922 +5pp ratchet target 83%... safe slack against the 84% meas |
| `scripts/ci/coverage-check.sh:36` | PER_FILE_MIN override for core/src/dnn/dnn_api.c = 83% | ADR-0114 baseline 78%; ratcheted +5pp to 83% by ADR-0922 (2026-05-31) |
| `scripts/ci/coverage-check.sh:38` | PER_FILE_MIN override for core/src/dnn/tiny_extractor_template.h = 75% | Ratcheted 10 → 75 on 2026-05-30 (ADR-0881)... 2.4 pp slack against 77.4% measurement... AD |
| `scripts/ci/coverage-delta-check.sh:17` | MAX_OVERALL_DROP=0.5, MAX_FILE_DROP=0.5 coverage drop tolerance | ADR-0922: per-PR drop tolerance (default 0.5pp on overall and per-touched-file) |
| `scripts/ci/cross_backend_parity_gate.py:173` | FEATURE_TOLERANCE relaxed contract for ciede (5e-3), psnr_hvs (5e-4), ssimulacra2 (5e-3) | Transcendentals / DCT — relaxed contract per ADR-0187 / ADR-0188 / ADR-0192 |
| `scripts/ci/gpu_ulp_calibration.yaml:43` | gpu_ulp_calibration.yaml per-GPU architecture tolerance overrides (e.g. DG2-G10 float_ssim = 5e-4) | ADR-0234, Research-0041: empirical fp32-rounding floor of that silicon + driver combinatio |
| `core/src/meson.build:26` | Compiler define -DVMAF_BUILDING_LIBVMAF | Suppresses VMAF_POOL_METHOD_NB deprecation attribute for internal translation units that l |
| `core/src/meson.build:978` | x86_avx512_lib override_options: ['b_lto=false'] | b_lto=false: with b_lto=true at meson.build top, LTO drops convolution_f32_avx512 symbols |
| `.github/codeql-config.yml:22` | CodeQL excludes cpp/poorly-documented-function query | ADR-0348: flags every C/C++ function not preceded by Doxygen block. Principles direct 'def |
| `.editorconfig:27` | [\*.{md,rst}] trim_trailing_whitespace = false | Markdown syntax requires two trailing spaces for line breaks |
| `.gitattributes:33` | pkg/benchmark/testdata/\*.csv -text disables CRLF normalization | CSV golden fixtures are byte-for-byte expectations... -text disables normalisation so fixt |
| `.iwyu.imp:1` | .iwyu.imp maps system/toolchain private headers to public headers (bits/\*, ext/\*, cuda_runtime, sycl, immintrin, arm_neo | Standard include-what-you-use private header mapping configuration |

## In-source suppression counts per directory

| Directory | Count | Breakdown |
| --- | --- | --- |
| `core/src` | 193429 | 258 suppressions (#[allow(=9, #pragma GCC diagnostic ignored=6, NOLINT=53, NOLINTBEGIN=23, NOLINTNEXTLINE=167) |
| `core/src/feature/x86` | 23437 | 29 suppressions (#pragma GCC diagnostic ignored=6, NOLINT=1, NOLINTNEXTLINE=22) |
| `core/src/feature/cuda` | 21349 | 9 suppressions (NOLINTBEGIN=2, NOLINTNEXTLINE=7) |
| `core/src/sycl` | 3681 | 0 suppressions (none (0 suppressions)) |
| `core/src/hip` | 1555 | 0 suppressions (none (0 suppressions)) |
| `core/src/metal` | 2167 | 0 suppressions (none (0 suppressions)) |
| `core/tools` | 10132 | 11 suppressions (NOLINT=2, NOLINTBEGIN=1, NOLINTNEXTLINE=8) |
| `core/test` | 67646 | ignore=4, #pragma GCC diagnostic ignored=2, NOLINT=2, NOLINTBEGIN=4, NOLINTNEXTLINE=58) |
| `python` | 40091 | 12 suppressions (# noqa=7, nosemgrep=5) |
| `compat` | 64920 | ignore=2, nosemgrep=10) |
| `ai` | 75325 | ignore=128) |
| `mcp-server` | 14832 | ignore=18) |
| `cmd` | 39590 | nolint=18) |
| `pkg` | 113412 | nolint=19) |
| `internal` | 1323 | 0 suppressions (none (0 suppressions)) |
| `rust` | 2267 | 10 suppressions (#[allow(=10) |
| `scripts` | 18908 | ignore=3) |

## Suppression families: ADR-citation compliance

| Suppression Family | With ADR Citation | No ADR Citation | Total | % ADR Citation |
| --- | --- | --- | --- | --- |
| `NOLINTNEXTLINE` | 167 | 127 | 294 | 56.8% |
| `NOLINTBEGIN` | 42 | 37 | 79 | 53.2% |
| `NOLINT` | 151 | 170 | 321 | 47.0% |
| `// cppcheck-suppress` | 0 | 0 | 0 | 0.0% |
| `#pragma GCC diagnostic ignored` | 0 | 14 | 14 | 0.0% |
| `#pragma clang diagnostic ignored` | 0 | 0 | 0 | 0.0% |
| `# noqa` | 7 | 338 | 345 | 2.0% |
| `# nosec` | 3 | 122 | 125 | 2.4% |
| `# type: ignore` | 5 | 253 | 258 | 1.9% |
| `# shellcheck disable` | 0 | 39 | 39 | 0.0% |
| `nosemgrep` | 0 | 27 | 27 | 0.0% |
| `//go:nolint` | 3 | 47 | 50 | 6.0% |
| `#[allow(` | 4 | 19 | 23 | 17.4% |

## What ADR-1142's PR does with this

- Bounds the whole CPU tree with the ratchet (every TU of the CPU build, 5,329
  warnings / 91 uncited NOLINTs at landing) and commits cuda/sycl/hip baselines.
- Retires the nightly `|| true` full scan (now the cpu-lane ratchet, fails on
  drift).
- Lists every remaining `NEEDS-TOOLCHAIN` row with its owner in
  [docs/development/ci.md](../development/ci.md#carve-outs-still-open-after-adr-1142);
  each `REMOVE` row is a fix-the-code item for the rework waves and shows up
  as a
  baseline decrease when done.
