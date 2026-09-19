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

## Governing ADRs

- [ADR-0006](../../docs/adr/0006-cli-precision-17g-default.md) — precision default.
- [ADR-0024](../../docs/adr/0024-netflix-golden-preserved.md) — Netflix goldens (Python-side).
- [ADR-0026](../../docs/adr/0026-workspace-relocated-under-python.md) — workspace relocation.
- [ADR-0029](../../docs/adr/0029-resource-tree-relocated.md) — resource tree relocation.
- [ADR-0030](../../docs/adr/0030-matlab-sources-relocated.md) — MATLAB source relocation.
- [ADR-0038](../../docs/adr/0038-purge-upstream-matlab-mex-binaries.md) — MEX binary purge.
- [ADR-1236](../../docs/adr/1236-version-single-source-tree.md) — single-source package versions and unify Python dependencies.
