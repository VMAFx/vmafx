<!-- markdownlint-disable MD060 -->
# Research digest — `__init__.py` export-completeness audit (2026-05-31)

**Companion ADR**: [ADR-0911](../adr/0911-init-py-export-completeness-audit.md)

## Question

For every `__init__.py` in the fork-added Python tree, does the file:

1. Carry the fork's Lusoris + SPDX header (CLAUDE.md §12 r7)?
2. Document its sub-modules in the module docstring?
3. Declare `__all__` as a machine-readable public-surface contract?

If not — fix the gaps in one focused PR, codify the pattern in an ADR
so future packages follow it.

## Method

1. `find . -name "__init__.py" -not -path "*/build*" -not -path
   "*/.venv/*" -not -path "*/node_modules/*"` — enumerate every package
   init in the working tree.
2. For each file: count lines, count `__all__` declarations, count
   `from .` re-exports.
3. Classify as **upstream-mirror** (Netflix copyright →
   leave byte-identical), **test-marker** (empty by convention →
   leave alone), or **fork-added** (in scope for the audit).
4. For each fork-added file, inspect the docstring vs. the actual
   sibling `.py` files; note staleness.
5. Spot-check `from <pkg> import *` callers to confirm no behavioural
   regression from adding `__all__`.
6. Verify each modified package imports cleanly with the right
   `PYTHONPATH`.

## Audit results

### In scope (fork-added)

| File | Lines | `__all__`? | Re-exports | Header? | Verdict |
|---|---:|:---:|---:|:---:|---|
| `ai/__init__.py` | 12 | no | 0 | yes | **fix** — add `__all__`, expand docstring with sub-package list |
| `ai/data/__init__.py` | 19 | no | 0 | yes | **fix** — add `__all__` (docstring already enumerates sub-modules) |
| `ai/train/__init__.py` | 16 | no | 0 | yes | **fix** — add `__all__`; **also**: docstring was stale (3 of 6 sub-modules listed) |
| `ai/src/vmaf_train/__init__.py` | 3 | no | 0 | **no** | **fix** — add SPDX header + expanded docstring + `__all__ = ["__version__"]` |
| `ai/src/vmaf_train/data/__init__.py` | 1 | no | 0 | **no** | **fix** — add SPDX header + sub-module list + `__all__` |
| `dev-llm/src/vmaf_dev_llm/__init__.py` | 3 | no | 0 | **no** | **fix** — add SPDX header + docstring + `__all__ = ["__version__"]` |
| `mcp-server/vmaf-mcp/src/vmaf_mcp/__init__.py` | 3 | no | 0 | **no** | **fix** — add SPDX header + docstring + `__all__ = ["__version__"]` |
| `scripts/lib/__init__.py` | 8 | no | 0 | **no** | **fix** — add SPDX header + `__all__` |

### Out of scope (upstream-mirror — leave byte-identical)

| File | Reason |
|---|---|
| `compat/python-vmaf/__init__.py` (378 lines) | Netflix copyright header; bulk of the upstream Python harness — rebase-sensitive |
| `compat/python-vmaf/core/__init__.py` | Netflix copyright (`Copyright 2016-2020, Netflix, Inc.`) |
| `compat/python-vmaf/tools/__init__.py` | Netflix copyright |
| `compat/python-vmaf/script/__init__.py` | upstream-mirror tree |
| `compat/python-vmaf/third_party/__init__.py` | upstream-mirror tree |
| `compat/python-vmaf/third_party/xiph/__init__.py` | upstream-mirror tree |
| `python/vmaf/__init__.py` (27 lines) | Fork-added compatibility shim — deliberately re-imports via `sys.modules` manipulation; adding `__all__` would not be meaningful (it re-imports the compat package wholesale) |

### Out of scope (test-marker, empty by convention)

| File | Reason |
|---|---|
| `python/test/__init__.py` (0 lines) | Pytest discovery marker; upstream-mirror anyway |
| `ai/tests/__init__.py` (2 lines, header only) | Pytest discovery marker — adding `__all__` is busywork |
| `tools/external-bench/tests/__init__.py` (0 lines) | Pytest discovery marker |

### Already well-formed (no change)

| File | `__all__` size | Notes |
|---|---:|---|
| `ai/src/aiutils/__init__.py` | 12 entries | Re-exports concrete symbols; uses `__getattr__` for lazy-import; reference example |
| `ai/src/corpus/__init__.py` | 10 entries | Re-exports concrete symbols from `.base` |
| `ai/src/vmaf_train/models/__init__.py` | 4 entries | Re-exports model classes |
| `tools/vmaf-tune/src/vmaftune/__init__.py` | 1 entry block | Pinned schema-version constants + canonical-6 feature names |
| `tools/vmaf-roi-score/src/vmafroiscore/__init__.py` | 4 entries | Pinned schema + result-key tuple |
| `tools/vmaf-tune/src/vmaftune/codec_adapters/__init__.py` | 21 re-exports | Codec-adapter dispatch table |

## Star-import safety check

```bash
grep -rEn "from (ai|ai\.data|ai\.train|vmaf_train|vmaf_dev_llm|vmaf_mcp|scripts\.lib) import \*" \
    --include="*.py" . | grep -v -E "(\.venv|\.cache|build/)"
```

→ **zero hits**. Adding `__all__` to a package that previously had none
is a behavioural no-op when no caller does `from pkg import *`. The
audit confirmed no caller does.

## Import-sanity check (after edits)

```text
PYTHONPATH=.        python3 -c "import ai; print(ai.__all__)"
    → ['data', 'train']
PYTHONPATH=.        python3 -c "import ai.data; print(ai.data.__all__)"
    → ['feature_extractor', 'netflix_loader', 'scores']
PYTHONPATH=.        python3 -c "import ai.train; print(ai.train.__all__)"
    → ['dataset', 'eval', 'konvid_pair_dataset', 'qat', 'train', 'train_combined']
PYTHONPATH=ai/src   python3 -c "import vmaf_train; print(vmaf_train.__version__)"
    → 0.1.0
PYTHONPATH=ai/src   python3 -c "import vmaf_train.data; print(vmaf_train.data.__all__)"
    → ['datasets', 'feature_dump', 'frame_dataset', 'frame_loader', 'manifest_scan', 'splits']
PYTHONPATH=dev-llm/src
                    python3 -c "import vmaf_dev_llm; print(vmaf_dev_llm.__version__)"
    → 0.1.0
PYTHONPATH=mcp-server/vmaf-mcp/src
                    python3 -c "import vmaf_mcp; print(vmaf_mcp.__version__)"
    → 0.1.0
PYTHONPATH=.        python3 -c "import scripts.lib; print(scripts.lib.__all__)"
    → ['backlog_tracker']
```

All eight modified packages import cleanly and expose the expected
`__all__` / `__version__` surface.

## Stale-docstring note (`ai/train/__init__.py`)

The previous docstring listed 3 sub-modules; the directory actually
contained 6:

- `dataset.py` (listed)
- `konvid_pair_dataset.py` (**not listed**)
- `qat.py` (**not listed**)
- `train_combined.py` (**not listed**)
- `eval.py` (listed)
- `train.py` (listed)

Per the user-scope "fix pre-existing inaccuracies in files you touch"
rule (CLAUDE.md memory: `feedback_fix_preexisting_bugs_too.md`), the
docstring is refreshed in the same change to enumerate all six.

## Findings summary

- 8 fork-added `__init__.py` files needed work; all fixed in this PR.
- 3 of those 8 also missed the Lusoris + SPDX header (CLAUDE.md §12 r7
  pre-existing debt — fixed).
- 1 of those 8 also had a stale docstring (3 of 6 sub-modules listed —
  fixed).
- 0 callers use `from <pkg> import *` against the touched packages;
  the change is behaviourally a no-op for runtime behaviour, but adds
  a machine-readable contract for pyright / IDE auto-import / future
  consumers.
- 6 fork-added packages were **already well-formed**; the audit
  confirms the existing convention and ADR-0911 codifies it.

## Decision

See [ADR-0911](../adr/0911-init-py-export-completeness-audit.md).
