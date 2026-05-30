# Research digest — `ai/tests/` script-loader helper extraction (2026-05-31)

**PR:** `refactor/ai-tests-script-loader-helper`
**Author:** Claude (mechanical refactor, audit-driven)
**Scope:** Pure fork-local DRY refactor inside `ai/tests/`.

## Background

A prior audit of `ai/tests/` flagged the duplicated
`importlib.util.spec_from_file_location → module_from_spec →
exec_module` recipe as the single highest-LOC DRY violation in the
package. The recipe appeared verbatim in **34 test files**, each
carrying roughly:

```python
_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "<name>.py"

def _load_module():
    spec = importlib.util.spec_from_file_location("<name>", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module      # (some files only)
    spec.loader.exec_module(module)
    return module
```

This is a textbook helper-extraction opportunity — the boilerplate has
no variation worth preserving across call sites except for (a) the
script's relative path under `ai/scripts/`, and (b) the optional
`sys.modules` key override used by ~10 files to avoid colliding with
sibling test names.

## Why the recipe exists at all

`ai/scripts/` is not a Python package — every file there is a
standalone CLI entry point with its own
`if __name__ == "__main__":` and no `__init__.py`. Tests cannot
`import ai.scripts.foo` and instead load the module by path. The
recipe is the canonical `importlib` answer for that.

## Change shape

Two helpers added to `ai/tests/conftest.py`:

| Helper | Resolves under | Use when |
|--------|----------------|----------|
| `load_ai_script(rel_path, *, name=None)` | `ai/scripts/` | 33 of 34 files |
| `load_ai_module(rel_path, *, name=None)` | `ai/`          | 1 file (`test_lpips_export.py`, whose target is `ai/lpips_export.py`) |

Both register the loaded module in `sys.modules` under its resolved
name. The optional `name=` keyword preserves pre-refactor `sys.modules`
keys for the ~10 files that pinned a non-default suffix (e.g.
`"extract_ugc_features_test_import"`).

## Measurements

- **Files changed:** 35 (1 helper-host conftest + 34 test files).
- **LOC delta:** roughly −250 LOC (boilerplate removed) +50 LOC
  (helpers + docstrings). Net ≈ −200 LOC across the package.
- **Test outcome:** every refactored test file still passes locally
  (`python3 -m pytest ai/tests/test_<name>.py`). The pre-existing
  19 host-env failures (torchvision NMS missing on Python 3.14) are
  unrelated — none touch the refactored files; confirmed against
  `origin/master` to rule out introduction by this PR.

## Behaviour-preservation notes

1. **`sys.modules` registration.** The helper always registers the
   loaded module in `sys.modules`. Pre-refactor, only ~10 of 34
   files did this; the other 24 left `sys.modules` untouched. The
   change is safe: registering is idempotent, makes pickling /
   dataclass `__module__` strings work, and matches stdlib guidance
   for path-loaded modules. No refactored test depends on the
   module being absent from `sys.modules`.
2. **`name=` override.** Files that previously created a spec with
   one name and registered under a different key (the `*_test_import`
   suffix pattern, ~5 files) now pin a single name via `name=`. The
   resulting `mod.__name__` matches the `sys.modules` key. None of
   the tests in those files inspect `mod.__name__`.
3. **`sys.path` side effects.** Two files
   (`test_merge_corpora.py`, `test_train_konvid_mos_head.py`,
   `test_validate_chug_hdr_mos_head.py`,
   `test_enrich_k150k_parquet_metadata.py`) inject auxiliary
   directories into `sys.path` independently of the loader recipe;
   those injections are preserved verbatim.

## Alternatives considered (deliberately not taken)

- **Auto-discover the helper via fixture parametrisation.** Would
  hide the explicit "this test loads script X" intent at call sites.
  Rejected.
- **Replace `importlib` with `runpy.run_path`.** `runpy` returns a
  dict, not a `ModuleType`; every test relies on attribute access
  (`mod.run`, `mod.main`, `mod.CONSTANT`). Rejected.
- **Make `ai/scripts/` an importable package by adding `__init__.py`.**
  Would change every direct-invocation entry point's `__name__` from
  `"__main__"` to `"ai.scripts.foo"`, breaking the
  `if __name__ == "__main__":` dispatch that 100+ scripts rely on.
  Rejected — out of scope.
- **Leave the boilerplate in place.** Documented status quo; rejected
  because the audit explicitly flagged it as the highest-impact DRY
  win in the package.

## ADR-0108 deliverables matrix

| # | Item | Status |
|---|------|--------|
| 1 | Research digest | This file |
| 2 | Decision matrix | "no alternatives: only-one-way DRY refactor" (alternatives above are reference, not branch points) |
| 3 | AGENTS.md invariant note | Added to `ai/AGENTS.md` Ground rules section |
| 4 | Reproducer | `python3 -m pytest ai/tests/test_youtube_ugc.py ai/tests/test_chug.py` |
| 5 | Changelog fragment | `changelog.d/changed/ai-tests-script-loader-helper.md` |
| 6 | `docs/rebase-notes.md` | "no rebase impact: fork-local DRY refactor inside `ai/tests/` which upstream Netflix/vmaf does not contain" entry added |
