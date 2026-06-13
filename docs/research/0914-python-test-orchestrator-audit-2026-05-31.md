<!-- markdownlint-disable MD060 -->
# Research-0914: Python test orchestrator audit (2026-05-31)

## Question

Does this repo have a unified Python test orchestrator (`tox.ini` /
`noxfile.py`) that covers every Python package under `ai/`,
`mcp-server/`, `tools/`, and `python/`? Should it?

## Method

1. Searched the working tree for `tox.ini`, `noxfile.py`, and
   `[tool.tox]` / `[tool.nox]` blocks in every `pyproject.toml`.
2. Catalogued every `pyproject.toml` and its `requires-python` pin.
3. Inspected `Makefile` and `.github/workflows/` for the canonical
   Python test invocations CI uses today.

## Findings

### Existing orchestrator coverage

- `python/tox.ini` exists. Covers **only** the legacy harness
  (`compat/python-vmaf` shim). Drives `py311` + `coverage` envs.
  Required by `make cythonize` + the Netflix CPU golden-data gate;
  CI runs it via `tox -c python` from `.github/workflows/build.yml`
  (lines 414 – 429) and `libvmaf-build-matrix.yml` (lines 681 – 696).
- No `noxfile.py` anywhere in the tree.
- No `[tool.tox]` / `[tool.nox]` blocks in any `pyproject.toml`
  (8 files checked).

### Python packages without an orchestrator

| Package | Path | `requires-python` | Test runner used in CI |
|---|---|---|---|
| Tiny-AI training | `ai/` | 3.11 – 3.14 | Ad-hoc venv + pytest in `tests-and-quality-gates.yml` lines 349 – 356 |
| MCP server | `mcp-server/vmaf-mcp/` | 3.10+ | Ad-hoc venv + pytest, lines 435 – 460 |
| vmaf-tune | `tools/vmaf-tune/` | 3.10 – 3.14 | Direct pytest (no dedicated CI job audited; runs via pre-commit + manual) |
| vmaf-roi-score | `tools/vmaf-roi-score/` | 3.10 – 3.12 | Direct pytest |
| ensemble-training-kit | `tools/ensemble-training-kit/` | 3.12 | Direct pytest |
| dev-llm | `dev-llm/` | 3.11+ | Direct pytest |

### Why CI works without a unified orchestrator

CI lanes have hard-coded venv + pip-install + pytest recipes per
package. They drift in lockstep with each `pyproject.toml` change. A
developer reproducing a specific lane locally has to read the YAML and
copy-paste the recipe.

### Why a unified orchestrator helps anyway

- `nox -l` becomes the discoverable index for every Python suite.
- `nox -s ai` (or `mcp`, `vmaf_tune`, …) replaces a 5-line venv recipe
  with one command.
- The orchestrator is a single dev-experience source of truth even
  though CI keeps its independent recipes (matrix reasons — different
  Python pins per package).

## Decision

Add `noxfile.py` at the repo root with one session per Python package
(`ai`, `mcp`, `vmaf_tune`, `dev_llm`, `roi_score`, `ensemble_kit`,
`python_harness`) plus meta-sessions `all` and `lint`. Use **nox** over
tox per ADR-0914 § Alternatives considered (nox composes better across
N packages with different `requires-python` ranges and conflicting
heavy deps; tox's INI config is denser per added package; a Makefile
loses the throw-away-venv isolation).

Keep `python/tox.ini` untouched (Cython + Netflix golden-data gate
specifics live there). The `python_harness` nox session shells out to
`tox -c python` rather than re-implementing the build steps.

CI keeps its existing per-package pytest invocations — **nox is not a
CI gate**, it is a local dev affordance.

## Verification

```text
$ nox -l -f noxfile.py
Sessions defined in /tmp/wt-tox/noxfile.py:
- ai             -> Run the ``ai/`` package pytest suite ...
- mcp            -> Run the ``mcp-server/vmaf-mcp/`` pytest suite.
- vmaf_tune      -> Run the ``tools/vmaf-tune/`` pytest suite.
- dev_llm        -> Run the ``dev-llm/`` pytest suite ...
- roi_score      -> Run the ``tools/vmaf-roi-score/`` pytest suite.
- ensemble_kit   -> Run the ``tools/ensemble-training-kit/`` pytest suite.
- python_harness -> Delegate to the legacy ``python/tox.ini`` harness.
* all            -> Run every per-package pytest suite in sequence.
* lint           -> Run ruff + black + isort in check-only mode.
```

All 9 sessions list correctly. Actual session execution (e.g. `nox -s
ai`) is deferred to the per-package pytest gate; nox parsing + session
discovery is the part this PR verifies.

## References

- Source: `req` (user request to audit Python test orchestration on
  2026-05-31).
- ADR-0914 — Unified Python test orchestrator decision record.
- `docs/development/python-test-orchestrator.md` — operator guide.
