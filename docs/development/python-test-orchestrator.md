<!-- markdownlint-disable MD060 -->
# Python test orchestrator (nox)

The fork ships multiple Python distributions plus compatibility regression
suites with different setup needs. To avoid memorising each recipe, the repo
has a top-level [`noxfile.py`](../../noxfile.py) that exposes each suite as a
named session.

Nox is a **local-developer affordance**, not a CI gate. CI continues to
run each package's pytest through its own `python3 -m venv ... && pip
install -e .[dev] && pytest tests/` recipe in
`.github/workflows/tests-and-quality-gates.yml`; the compatibility decorator
suite also runs on every OS in [build.yml](../../.github/workflows/build.yml)
to exercise the native POSIX and Windows lock implementations. The decision
record is [ADR-0914](../adr/0914-unified-python-test-orchestrator.md).

## Install

```bash
python3 -m pip install --require-hashes -r requirements/locks/nox.txt
```

Nox creates its own per-session venvs by default
(`.nox/<session-name>/`) — you do not need to pre-create one. With
`nox.options.reuse_existing_virtualenvs = True` set in the noxfile,
re-runs reuse the environment while still reconciling its locked installs. Use
Nox's `-R` option only when you intentionally want to reuse the environment and
skip installation.

Every session installs a manifest-owned hash lock first, then installs the local
package with `--no-deps --no-build-isolation`. Nox may download a requested
standalone Python interpreter; `roi_score` and `ensemble_kit` request Python 3.12
because their package metadata excludes Python 3.14.

## Sessions

| Session | Target | Notes |
|---|---|---|
| `ai` | `ai/tests/`, `ai/sidecar/tests/` | Tiny-AI training scripts and online-training sidecar. Heavy: pulls `torch`, `lightning`. |
| `mcp` | `mcp-server/vmaf-mcp/tests/` | MCP JSON-RPC server. |
| `vmaf_tune` | `tools/vmaf-tune/tests/` | Encode-tuning harness. |
| `dev_llm` | `dev-llm/tests/` | Local-LLM helper (Ollama-backed). |
| `roi_score` | `tools/vmaf-roi-score/tests/` | Saliency-aware ROI tooling; Python 3.12. |
| `ensemble_kit` | `tools/ensemble-training-kit/tests/` | ONNX ensemble training; Python 3.12. |
| `compat_decorator` | `compat/vmaf/tests/test_decorator_extended.py` | SHA-256 memoization, recursion, thread/spawn concurrency, and native file locking. |
| `python_harness` | `python/tox.ini` | Delegates to legacy tox (Cython + golden-data). |
| `all` | every per-package suite | Excludes `python_harness` (needs C build). |
| `lint` | `python/`, `ai/`, `scripts/` | Ruff + Black, check-only. |

## Usage

```bash
nox -l                          # list every session with its docstring
nox -s ai                       # run ai/tests/ and ai/sidecar/tests/ in an isolated venv
nox -s mcp vmaf_tune            # run multiple suites in sequence
nox -s compat_decorator         # run the compatibility decorator regressions
nox -s python_harness           # invoke the legacy python/ tox harness
nox -s all                      # every fork-local Python package
nox -s lint                     # check-only Ruff + Black
nox -s ai -- -k test_smoke      # pass posargs through to pytest
```

The `--` separator forwards everything after it to the underlying
`pytest` invocation, so `-k`, `-x`, `--lf`, `--maxfail=N` and friends
work as usual.

## Adding a new Python package

When a new package lands under `ai/`, `mcp-server/`, `tools/`, or
similar, add **both**:

1. A new session in [`noxfile.py`](../../noxfile.py) following the
   existing one-per-package template, plus a dedicated development lock entry
   in [`manifest.json`](../../requirements/locks/manifest.json). Pin the session
   interpreter when the package's `requires-python` range excludes the Nox host.
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
