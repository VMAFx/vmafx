<!-- markdownlint-disable MD013 -->
# AGENTS.md — python/vmaf

Orientation for agents working on Python bindings and **classic**
(SVM-based) VMAF training / eval harness. Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

- Python bindings around libvmaf (`vmafrc`, `quality_runner`, …)
- Upstream Netflix training / analysis harness (SVM, MOS analysis, plots)
- Fork-local scratch and resource trees relocated from repo root

Not in scope: tiny-AI training -> lives in [../../ai/](../../ai/AGENTS.md).

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
- **Never commit Netflix golden-score changes.** Python-side golden
  assertions in [../test/](../test/) = numerical-correctness gate for
  VMAF. Run in CI as required status check. Never modified by any PR.
  See [ADR-0024](../../docs/adr/0024-netflix-golden-preserved.md).
- **Never commit MEX / compiled MATLAB binaries**: upstream shipped ~53
  `.mexa64` / `.dll` / `.o` / `.lib` artefacts in `matlab/`; purged
  2026-04-17, blocked by `.gitignore`. See
  [ADR-0038](../../docs/adr/0038-purge-upstream-matlab-mex-binaries.md).
  `.c` and `.m` sources stay — anyone needing MATLAB path rebuilds
  locally with `mex file.c`.
- **Workspace and resource paths go through `config.py` constants**
  (`WORKSPACE`, `RESOURCE`). Overridable via `VMAF_WORKSPACE` /
  `VMAF_RESOURCE` env vars. See
  [ADR-0026](../../docs/adr/0026-workspace-relocated-under-python.md),
  [ADR-0029](../../docs/adr/0029-resource-tree-relocated.md).
- **Precision**: `result.py` serialises floats at `%.6f` by default,
  matching CLI (Netflix-compat golden gate). See
  [ADR-0119](../../docs/adr/0119-cli-precision-default-revert.md)
  (supersedes [ADR-0006](../../docs/adr/0006-cli-precision-17g-default.md)).

## Rebase invariants

- **`PyPsnrFeatureExtractor` = primary class; `PypsnrFeatureExtractor` =
  `@deprecated` alias.** Future upstream sync touching
  `feature_extractor.py` around these classes -> verify hierarchy
  preserved:
  `PyPsnrFeatureExtractor(PyFeatureExtractorMixin, FeatureExtractor)`
  as primary (TYPE `"PyPsnr_feature"`),
  `PypsnrFeatureExtractor(PyPsnrFeatureExtractor)` as deprecated alias
  (TYPE `"Pypsnr_feature"`). Same pattern applies to
  `PyPsnrMaxdb100FeatureExtractor` / `PypsnrMaxdb100FeatureExtractor`.
  Upstream commit renaming or removing `Pypsnr*` aliases -> absorb
  without touching `PyPsnr*` primary names — test file asserts against
  those. Tracked: fix/pypsnr-feature-extractor-import PR (2026-05-10).
- **`routine.py::run_test_on_dataset()` only reads bootstrap score keys
  from bootstrap-capable runners.** Normal `VmafQualityRunner` /
  `PsnrQualityRunner` results do not expose `get_bagging_score_key()` /
  CI95 / all-model prediction fields; keep bootstrap kwargs conditional
  on full getter set. macOS tox lane runs `run_testing.py` through
  those normal runners -> unconditional bootstrap-key access regresses
  CLI tests before any score assertion executes.
- **Doctests must not depend on NumPy scalar `repr()` or assertion
  traceback details.** NumPy 2 may render scalar results as
  `np.float64(...)`; Python 3.14 appends assert-expression detail to
  `AssertionError` text. Cast numeric scalar examples to `float(...)`
  or format them; print only first exception-message line when
  doctest documents assertion text.
- **`tools/scanf.py::makeFormattedHandler.applyWidth` width guard
  swapped vs upstream.** Fork inverts upstream `if width is None`
  branches: implicit-width converters return unwrapped handler,
  explicit-width converters return capped wrapper. Without this,
  implicit `%d` / `%f` / `%s` / `%x` crashes inside `CappedBuffer` with
  `TypeError`; explicit `%5d` silently drops cap. Future upstream sync
  re-touching this function -> preserve swapped semantics, or confirm
  upstream independently applied same fix. Regression test:
  `python/test/python_harness_scanf_locale_bugs_test.py`. See
  [ADR-0955](../../docs/adr/0955-compat-python-vmaf-scanf-locale-bugs.md).
- **`ProcessRunner.run` forces `LC_ALL=C` / `LANG=C` unconditionally.**
  Fork builds base env from caller's `env=` kwarg (or `os.environ` if
  none), then stamps C-locale keys on top. Preserves caller-supplied
  env entries (e.g. `FFMPEG_ENV` paths) while guaranteeing English
  subprocess error messages on any host locale. Do not regress to
  `setdefault` when porting upstream changes — `setdefault` = no-op
  when parent shell already has `LANG=de_DE.UTF-8`, defeats intent
  entirely.
- **`tools/scanf.py` has latent inverted-width bug** at
  `makeFormattedHandler.applyWidth` (line 648 — `if width is None:`
  instead of `if width is not None:`). Implicit-width path (`%d`,
  `%f`, `%s` with no explicit width) crashes with TypeError;
  explicit-width path silently ignores cap. Only patterns with literal
  delimiters bounding capture for free work (`frame%08d.icpf` —
  `.icpf` ends digit run). Every in-tree caller
  (`tools/misc.check_scanf_match`, dataset / frame-name parsers) uses
  literal-delimited shape; do not add tests probing broken branches
  without fixing bug first. Flagged in PR
  `test/python-test-coverage-push` (round-2 coverage push).
- **`python/pyproject.toml [project].dependencies` = single source of
  Python dependencies (ADR-1236).** `python/setup.py` intentionally
  does NOT declare `install_requires=[...]`. Setuptools automatically
  loads `[project].dependencies` from `python/pyproject.toml`. Do not
  reintroduce `install_requires` during upstream merge or port.
  `python/requirements.txt` mechanically generated via
  `scripts/ci/check-python-requirements-single-source.sh --write`
  (`make python-deps-sync`); never edit manually. Renovate ignores
  `python/requirements.txt`, avoids duplicate PRs.
- **Python worker creation is fork-free (ADR-1278).**
  `tools.misc.parallel_map()` uses joblib's `loky` backend so local callables
  remain supported without inheriting live threads through POSIX `fork`.
  `Executor.run()` and `run_executors_in_parallel()` group equal
  `str(asset)` keys into serial work units and restore input order; do not
  replace that grouping with independently dispatched duplicates. FIFO helper
  processes use the explicit `spawn` context, one readiness semaphore per
  child, and bounded readiness waits. A child that exits before signaling must
  surface its role, exit code, and available traceback; never restore an unconditional
  `sem.acquire()` after the five-second warning. The classic 5PL curve in
  `core/train_test_model.py` keeps `b1` as the sigmoid amplitude and uses
  `scipy.special.expit`; additive `b1` is redundant with `b5` and a raw
  exponential overflows.
- **Pytest warnings are errors, without carve-outs (ADR-1278).** The root,
  package-local, and legacy `python/tox.ini` configs all promote every warning
  to an error. Never restore `-p no:warnings`, add an `ignore` warning filter,
  or weaken a CI invocation to make a warning-producing test pass. Fix the
  warning's cause instead. `tools.misc.import_python_file()` must close its
  override temporary file before reopening it and remove the path in `finally`;
  relying on garbage collection emits `ResourceWarning` on Python 3.14.

- **Object lifetime in the harness is explicit, never GC-driven
  (T-PY-HARNESS-OBJECT-LIFETIME-WARNINGS-2026-09-22).** Never write
  `tempfile.NamedTemporaryFile(...).name`: the expression drops the wrapper,
  and its finaliser emits `ResourceWarning` from whatever test the cyclic
  collector happens to interrupt. Use `tempfile.mkstemp()` and close the
  descriptor, or keep the object in a `with` block. The same finaliser hides
  in `urllib.error.HTTPError`: `urllib.response.addbase`, which it inherits
  through `addinfourl`, *is* `tempfile._TemporaryFileWrapper`, and CPython
  3.14 gives it an `io.BytesIO` when `fp is None` -- so a constructed
  `HTTPError` must be closed too. And never call sureal's
  `SubjectiveModel.from_dataset_file()` (or `PairedCompSubjectiveModel`'s
  override): it imports the dataset through
  `SourceFileLoader.load_module()`, which Python 3.15 removes and 3.12+ warns
  on. `routine.py` imports the dataset with
  `tools.misc._import_dataset_and_filter()` and constructs
  `subj_model_class(dataset_reader_class(dataset))` itself; an upstream merge
  that restores the `from_dataset_file()` call re-breaks the harness.

- **HISS-04 helpers are extraction, not redesign (T-HISS-PY-COMPAT-2026-09-21).**
  The oversized routines were split so each piece stays under the 60-LOC
  scanner bound, and the split points were chosen to keep observable output
  identical: `VmafFeatureExtractor` / `VmafIntegerFeatureExtractor` route
  options through `VMAF_FLOAT_FEATURE_OPTION_TARGETS` /
  `VMAF_INTEGER_FEATURE_OPTION_TARGETS` (an unlisted option is still ignored
  silently), `quality_runner._resolve_vmafexec_options()` keeps `models`
  deliberately unassigned for `use_default_built_in_model=False` because
  upstream does, `perf_metric` and `noref_feature_extractor` preserve the
  accumulation order of every reduction, and `local_explainer` /
  `nn_train_test_model` keep the per-sample RNG draw order so a seeded run
  reproduces. `result.scores_key_wildcard_match()`'s doctests were
  redistributed across the new helpers, not dropped — `doctest.testmod()`
  must still find all twelve.
- **Bounded frame loops replace `while True` (HISS-02).** The PyPSNR loop in
  `core/feature_extractor.py` walks `min(ref.num_frms, dis.num_frms)`, which
  `YuvReader` already validates as an exact count at construction; do not
  reintroduce a `try/except StopIteration` spin. `tools/scanf.py` carries its
  termination in the loop header; `readiter()` keeps its trailing
  `raise StopIteration` (a PEP 479 RuntimeError for callers) on purpose —
  changing that is a behaviour change, not a cleanup.
- **The MATLAB MEX sources are refactored in place (ADR-0030 / ADR-0038).**
  `matlab/strred/matlabPyrTools/MEX/` and `matlab/STMAD_2011_MatlabCode/`
  now carry `static` helpers instead of the long `INPROD` macro bodies and
  inline argument parsing. Every index expression, accumulation order and
  error string is unchanged; `edges[]` is filled with a bounded copy because
  HISS-08 bans `strcpy()`. See `docs/rebase-notes.md`.
- **Memoization cache key stability (SHA-256) (T-SEMGREP-WARNING-ALERTS-946-949-2026-09-23).**
  `tools/decorator.py` generates in-memory and on-disk cache keys in
  `@persist`, `@persist_to_file`, and `@persist_to_dir` using
  `hashlib.sha256(..., usedforsecurity=False)` per [ADR-1307](../../docs/adr/1307-sha256-memoization-cache-invalidation.md)
  (partially superseding ADR-1222 for its SHA-1 keep-open disposition). Clean cold invalidation of legacy caches
  was accepted as keys are runtime/ephemeral memoization only, completely decoupled
  from durable pipeline models or Netflix golden assertions. Thread concurrency is
  serialized via `threading.RLock()` (avoiding dynamic programming recursion deadlock),
  cross-process cache updates in `persist_to_file` are synchronized via re-entrant
  `_file_lock` (`fcntl.flock` on POSIX, `msvcrt.locking` on Windows) and disk cache
  reloading/merging (preventing multi-process and recursive cache clobbering), and atomic
  file writing is guaranteed via `tempfile.mkstemp` (avoiding
  PID collisions). Removing SHA-1 entirely yields 0 Semgrep findings in SARIF, resolving warning
  alerts 947–949 at source. Regression tests in `compat/vmaf/tests/test_decorator_extended.py`
  (26 tests passing) guard key length (64 hex characters), golden vectors, thread concurrency,
  cross-process concurrent updates, cross-process cache hits, Windows lock dispatch, and cold invalidation.

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
