<!-- markdownlint-disable MD060 -->
# Python test orchestrator (nox)

The fork ships eight Python distributions, each with its own
`pyproject.toml`, `tests/` directory, and `requires-python` range. To
avoid copy-pasting per-package venv recipes out of CI YAML, the repo
has a top-level [`noxfile.py`](../../noxfile.py) that exposes every
suite as a named session.

Nox is a **local-developer affordance**, not a CI gate. CI continues to
run each package's pytest through its own `python3 -m venv ... && pip
install -e .[dev] && pytest tests/` recipe in
`.github/workflows/tests-and-quality-gates.yml`. The decision record is
[ADR-0914](../adr/0914-unified-python-test-orchestrator.md).

## Install

```bash
pip install nox
```

Nox creates its own per-session venvs by default
(`.nox/<session-name>/`) — you do not need to pre-create one. With
`nox.options.reuse_existing_virtualenvs = True` set in the noxfile,
re-runs of the same session skip the install step.

## Sessions

| Session | Target | Notes |
|---|---|---|
| `ai` | `ai/tests/` | Tiny-AI training scripts. Heavy: pulls `torch`, `lightning`. |
| `mcp` | `mcp-server/vmaf-mcp/tests/` | MCP JSON-RPC server. |
| `vmaf_tune` | `tools/vmaf-tune/tests/` | Encode-tuning harness. |
| `dev_llm` | `dev-llm/tests/` | Local-LLM helper (Ollama-backed). |
| `roi_score` | `tools/vmaf-roi-score/tests/` | Saliency-aware ROI tooling. |
| `ensemble_kit` | `tools/ensemble-training-kit/tests/` | ONNX ensemble training. |
| `python_harness` | `python/tox.ini` | Delegates to legacy tox (Cython + golden-data). |
| `all` | every per-package suite | Excludes `python_harness` (needs C build). |
| `lint` | `python/`, `ai/`, `scripts/` | ruff + black + isort, check-only. |

## Usage

```bash
nox -l                          # list every session with its docstring
nox -s ai                       # run ai/tests/ in an isolated venv
nox -s mcp vmaf_tune            # run multiple suites in sequence
nox -s python_harness           # invoke the legacy python/ tox harness
nox -s all                      # every fork-local Python package
nox -s lint                     # check-only ruff + black + isort
nox -s ai -- -k test_smoke      # pass posargs through to pytest
```

The `--` separator forwards everything after it to the underlying
`pytest` invocation, so `-k`, `-x`, `--lf`, `--maxfail=N` and friends
work as usual.

## Adding a new Python package

When a new package lands under `ai/`, `mcp-server/`, `tools/`, or
similar, add **both**:

1. A new session in [`noxfile.py`](../../noxfile.py) following the
   existing one-per-package template.
2. A new job (or matrix entry) in
   [`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)
   that drives the same `pip install -e <path>[dev] && pytest <path>/tests/`
   sequence — CI does not call nox.

The PR template's deep-dive deliverables checklist will catch a
missing CI lane during review; nothing automatically catches a missing
nox session, so reviewer eyeball is the gate.

## Why nox and not tox

ADR-0914 § Alternatives considered. Briefly: tox's INI config does not
compose well across N packages with different `requires-python` ranges
and conflicting heavy deps (`torch` vs `optuna` vs `mcp`); a single
Makefile target loses the throw-away-venv isolation that the CI lanes
rely on; `pytest-xdist --rootdir` collapses dep trees that must stay
separate (torch + optuna co-resolve poorly).

## What nox does **not** do

- It does not call `meson` / `ninja` — the C build is out of scope.
  Use `make build` first if your suite needs a built `vmaf` binary.
- It does not run the Netflix CPU golden-data gate. That stays
  exclusively in `make test-netflix-golden` (which drives pytest
  directly against the legacy `python/test/` files).
- It does not replace `make lint`. The `lint` session is convenience;
  `make lint` remains the canonical CI invocation.
