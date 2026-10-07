<!-- markdownlint-disable MD013 -->
# AGENTS.md — python/vmaf

Python bindings and classic (SVM) VMAF harness. Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

- Python bindings for libvmaf (`vmafrc`, `quality_runner`, …)
- Upstream Netflix harness (SVM, MOS analysis, plots)
- Relocated scratch and resource trees

Tiny-AI training lives in [../../ai/](../../ai/AGENTS.md).

```text
python/vmaf/
  config.py              # WORKSPACE / RESOURCE constants + env overrides
  workspace/             # classic-harness scratch (gitignored subtrees except placeholders)
  resource/              # example datasets + param files
  matlab/                # MATLAB reference implementations (strred, SpEED, STMAD, cid_icid)
  …                      # bindings + harness modules
```

## Ground rules

- **Parent rules** apply: see [../../AGENTS.md](../../AGENTS.md).
- **Never commit Netflix golden-score changes.** Assertions in [../test/](../test/)
  = numerical gate. Required CI status check. Never modified (ADR-0024).
- **Never commit MEX / compiled MATLAB binaries**: ~53 `.mexa64` / `.dll` /
  `.o` / `.lib` artefacts in `matlab/` purged 2026-04-17 (`.gitignore`, ADR-0038).
  `.c` and `.m` sources stay; rebuild via `mex file.c`.
- **Paths via `config.py` constants** (`WORKSPACE`, `RESOURCE`). Overridable
  via `VMAF_WORKSPACE` / `VMAF_RESOURCE` (ADR-0026, ADR-0029).
- **Precision**: `result.py` serialises floats at `%.6f` matching CLI ([ADR-0119](../../docs/adr/0119-cli-precision-default-revert.md), supersedes ADR-0006).
- **Build directory overrides ([ADR-1317](../../docs/adr/1317-golden-gate-build-isolation.md))**: `__init__.py` respects `VMAF_BUILD_DIR`
  (defaults `core/build`) locating `tools/vmaf`. `config.py` provides `VMAF_PATH`
  and `VMAFEXEC_PATH` overrides. Used by `make test-netflix-golden` for `core/build-golden`.

## Rebase invariants

- **Feature discovery is Netflix's multi-nickname form (d327ed67b).**
  `VmafexecFeatureExtractorMixin` and `FeatureDiscoveryMixin` keep one
  `defaultdict(list)` per atom feature (nickname -> scores) and nickname
  list; wildcard collects every emitted key it owns, owner being
  longest atom feature that prefixes nickname. Never bring back fork's
  former "shortest key wins" single-nickname form, nor
  `CambiFullReferenceFeatureExtractor`'s `cambi_encbd` atom feature it needed:
  `python/test/cambi_test.py` (Netflix's file) asserts `cambi` keys.
  `assert_same_frame_count()` takes count from first non-empty
  nickname (Netflix's loop leaves it unset when feature 0 is absent).
- **`Asset.ORDERED_FILTER_LIST` is Netflix's order** (crop, pad, gblur, eq,
  lutyuv, yadif, format, fps, select; 560c4e491). It orders FFmpeg chain
  and asset string, so reorder changes workfiles and cached results.
- **`TrainTestModel.postprocess_feature_from_another()` returns Python floats**
  (`[float(v) ...]`, Netflix: `list(ndarray)`), for NumPy 2 doctest rule
  below; ResPow guard catches `Exception`, not Netflix's bare `except:`.
- **`PyPsnrFeatureExtractor` primary; `PypsnrFeatureExtractor` `@deprecated` alias.**
  Hierarchy: `PyPsnrFeatureExtractor(PyFeatureExtractorMixin, FeatureExtractor)`
  (TYPE `"PyPsnr_feature"`), `PypsnrFeatureExtractor(PyPsnrFeatureExtractor)`
  (TYPE `"Pypsnr_feature"`). Same for `PyPsnrMaxdb100FeatureExtractor` /
  `PypsnrMaxdb100FeatureExtractor`. Upstream changes renaming/removing `Pypsnr*`
  absorbed without touching `PyPsnr*` names (asserted in tests). Tracked:
  `fix/pypsnr-feature-extractor-import` PR (2026-05-10).
- **`routine.py::run_test_on_dataset()` reads bootstrap keys on bootstrap runners only.**
  Normal `VmafQualityRunner` / `PsnrQualityRunner` results omit `get_bagging_score_key()` /
  CI95 / all-model prediction; keep bootstrap kwargs conditional. macOS tox lane
  runs `run_testing.py` through normal runners.
- **Doctests must not depend on NumPy scalar `repr()` or traceback details.**
  NumPy 2 renders `np.float64(...)`; Python 3.14 appends assert details to `AssertionError`.
  Cast scalars to `float(...)`; print first line of exception message.
- **`tools/scanf.py::makeFormattedHandler.applyWidth` width guard swapped vs upstream.**
  Fork inverts upstream `if width is None:` branches: implicit-width returns
  unwrapped handler, explicit-width returns capped wrapper. Without inversion,
  implicit `%d` / `%f` / `%s` / `%x` crashes in `CappedBuffer` with `TypeError`;
  explicit `%5d` drops cap. Preserve swapped semantics ([ADR-0955](../../docs/adr/0955-compat-python-vmaf-scanf-locale-bugs.md), test
  `python/test/python_harness_scanf_locale_bugs_test.py`).
- **`ProcessRunner.run` forces `LC_ALL=C` / `LANG=C` unconditionally.**
  Builds env from `env=` kwarg (or `os.environ`), stamps C-locale keys.
  Preserves caller env (`FFMPEG_ENV`) while ensuring English errors. Never
  revert to `setdefault` (no-op on `LANG=de_DE.UTF-8`).
- **`tools/scanf.py` latent inverted-width bug** at `makeFormattedHandler.applyWidth`
  (line 648 — `if width is None:` instead of `if width is not None:`).
  Implicit `%d`, `%f`, `%s` crashes TypeError; explicit ignores cap. Only literal
  delimiters work (`frame%08d.icpf` — `.icpf` ends digits). In-tree callers
  (`tools/misc.check_scanf_match`, dataset/frame parsers) use literal delimiters;
  do not probe broken branches without fix (PR `test/python-test-coverage-push`).
- **`python/pyproject.toml [project].dependencies` single dependency source (ADR-1236).**
  `python/setup.py` omits `install_requires=[...]`; Setuptools loads
  `[project].dependencies` from `python/pyproject.toml`. Never reintroduce
  `install_requires`. `python/requirements.txt` generated via
  `scripts/ci/check-python-requirements-single-source.sh --write`
  (`make python-deps-sync`). Renovate ignores `python/requirements.txt`.
- **Python worker creation fork-free (ADR-1278).** `tools.misc.parallel_map()`
  uses joblib `loky` backend. `Executor.run()` and `run_executors_in_parallel()`
  group equal `str(asset)` keys into serial work units, preserving order.
  FIFO helpers use `spawn`, 1 readiness semaphore per child, bounded waits.
  Exiting child surfaces role, exit code, traceback; never restore unconditional
  `sem.acquire()` after 5 s warning. FIFO error delivery failure closes pipe,
  attaches context via `_safe_add_exception_note` (bypasses overridden `add_note`,
  suppresses `BaseException`-derived control failures, `_run_fifo_worker`);
  `_fifo_worker_failure` treats EOF as failure, not `None`.
  `RegressorMixin._get_scatter_arrays` uses `is None` not `== None`. 5PL curve in
  `core/train_test_model.py` keeps `b1` as sigmoid amplitude via `scipy.special.expit`;
  additive `b1` redundant with `b5`, raw exp overflows.
- **Pytest warnings are errors without carve-outs (ADR-1278).** Root, package-local,
  legacy `python/tox.ini` configs promote warnings to errors. Never add `-p no:warnings`
  or `ignore` filter. `tools.misc.import_python_file()` closes temporary file before
  reopening; removes path in `finally` (avoids `ResourceWarning` on Python 3.14).
- **Harness object lifetime explicit, not GC-driven (T-PY-HARNESS-OBJECT-LIFETIME-WARNINGS-2026-09-22).**
  Never use `tempfile.NamedTemporaryFile(...).name` (finaliser emits `ResourceWarning`).
  Use `tempfile.mkstemp()` and close fd, or use `with`. In `urllib.error.HTTPError`,
  `urllib.response.addbase` inherits `addinfourl` (`tempfile._TemporaryFileWrapper`),
  CPython 3.14 gives `io.BytesIO` when `fp is None` (close `HTTPError`). Never call sureal
  `SubjectiveModel.from_dataset_file()` or `PairedCompSubjectiveModel` override
  (`SourceFileLoader.load_module()` removed Python 3.15, warns 3.12+). `routine.py`
  imports via `tools.misc._import_dataset_and_filter()`, constructs
  `subj_model_class(dataset_reader_class(dataset))` directly.
- **HISS-04 helpers: extraction, not redesign (T-HISS-PY-COMPAT-2026-09-21).**
  Routines split under 60 LOC bound; output identical: `VmafFeatureExtractor` /
  `VmafIntegerFeatureExtractor` route options through `VMAF_FLOAT_FEATURE_OPTION_TARGETS` /
  `VMAF_INTEGER_FEATURE_OPTION_TARGETS`; `quality_runner._resolve_vmafexec_options()` keeps
  `models` unassigned for `use_default_built_in_model=False`; `perf_metric` and
  `noref_feature_extractor` preserve reduction accumulation order; `local_explainer` /
  `nn_train_test_model` keep RNG draw order. `result.scores_key_wildcard_match()`
  doctests redistributed across helpers; `doctest.testmod()` finds all 12.
- **`__init__.py` command builders are assembled from pure helpers
  (`T-PYTHON-CALL-VMAFEXEC-FORCE-ZERO-SECOND-MODEL-2026-10-02`).**
  `ExternalProgramCaller.call_vmafexec()` and `call_vmafexec_multi_features()`
  keep their signatures; command text comes from
  `_vmafexec_base_command`, `_vmafexec_feature_flags`, `_vmafexec_model_flags`
  (which calls `_vmafexec_model_overloads` once per model),
  `_vmafexec_run_flags`, `_multi_features_run_arguments` and
  `_feature_argument`. Keep helpers pure: upstream's loop overwrote
  `motion_force_zero` with string `"true"` and then failed its own
  `isinstance(..., bool)` assertion on second model. upstream change to
  flag goes into helper that emits it; order of parts is pinned
  by `test_full_command_is_pinned` and
  `test_full_multi_features_command_is_pinned` in
  `python/test/python_harness_coverage_test.py`.
- **Bounded frame loops replace `while True` (HISS-02).** PyPSNR loop in
  `core/feature_extractor.py` walks `min(ref.num_frms, dis.num_frms)` (`YuvReader` validated);
  no `try/except StopIteration`. `tools/scanf.py` bounds loop in header; `readiter()`
  keeps trailing `raise StopIteration` (PEP 479 RuntimeError).
- **MATLAB MEX sources refactored in place (ADR-0030 / ADR-0038).**
  `matlab/strred/matlabPyrTools/MEX/` and `matlab/STMAD_2011_MatlabCode/` carry `static`
  helpers instead of `INPROD` macros and inline arg parsing. Indices, accumulation, errors
  unchanged; `edges[]` bounded copy (HISS-08 bans `strcpy()`). See `docs/rebase-notes.md`.
- **Memoization cache key stability (SHA-256) (T-SEMGREP-WARNING-ALERTS-946-949-2026-09-23).**
  `tools/decorator.py` generates cache keys in `@persist`, `@persist_to_file`,
  `@persist_to_dir` via `hashlib.sha256(..., usedforsecurity=False)` per ADR-1307
  (supersedes ADR-1222 keep-open). runtime/ephemeral memoization decoupled from durable pipeline
  or golden assertions. Threads serialized via `threading.RLock()`; cross-process
  updates in `persist_to_file` synchronized via re-entrant `_file_lock` (`fcntl.flock` POSIX,
  `msvcrt.locking` Windows) and cache reloading/merging; atomic file writing via
  `tempfile.mkstemp`. 0 Semgrep findings in SARIF, resolving alerts 947–949. Tests in
  `compat/vmaf/tests/test_decorator_extended.py` (26 tests) guard key length (64 hex),
  golden vectors, concurrency, Windows lock dispatch, cold invalidation.
- **`python/pyproject.toml` declares licences `vmaf` wheel carries ([ADR-1560](../../docs/adr/1560-python-package-licence-union.md)).**
  `BSD-2-Clause-Patent AND BSD-2-Clause AND BSD-3-Clause-Clear AND EUPL-1.2`
  with `license-files = ["LICENSES/*"]` (`python/LICENSES/`), not upstream's
  `BSD-2-Clause-Patent`: modules of this tree carry BSD-2-Clause and
  BSD-3-Clause-Clear, and ADM extension includes EUPL-1.2 headers. new
  file under another licence here, or new header `adm_dwt2_cy.pyx` pulls in,
  fails `python/test/setup_metadata_test.py` until expression and texts
  gain it.
- **`SubjectiveDatasetReader` / `SubjectiveDatasetTester` are Netflix's API
  over fork's helpers (2e6bbb657).** `read_dataset()` /
  `run_test_on_dataset()` wrap them. Port Netflix changes of `read()` / `run()`
  into `_resolve_asset_fields()`, `_build_asset_dict()`,
  `_tester_optional_dict()` and friends; never paste long methods back
  (HISS-04). tester keeps `allow_uncalibrated` (ADR-0620).

## Governing ADRs

- [ADR-0006](../../docs/adr/0006-cli-precision-17g-default.md) — precision default.
- [ADR-0024](../../docs/adr/0024-netflix-golden-preserved.md) — Netflix goldens (Python-side).
- [ADR-0026](../../docs/adr/0026-workspace-relocated-under-python.md) — workspace relocation.
- [ADR-0029](../../docs/adr/0029-resource-tree-relocated.md) — resource tree relocation.
- [ADR-0030](../../docs/adr/0030-matlab-sources-relocated.md) — MATLAB source relocation.
- [ADR-0038](../../docs/adr/0038-purge-upstream-matlab-mex-binaries.md) — MEX binary purge.
- [ADR-1236](../../docs/adr/1236-version-single-source-tree.md) — single-source package versions and unify Python dependencies.
- [ADR-1278](../../docs/adr/1278-python-safe-parallel-execution.md) — fork-free process execution and reference 5PL fitting.
- [ADR-1307](../../docs/adr/1307-sha256-memoization-cache-invalidation.md) — SHA-256 memoization cache key upgrade and clean cold invalidation.
