<!-- markdownlint-disable MD060 -->
# ADR-0914: Unified Python test orchestrator (nox at repo root)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris, Claude
- **Tags**: `build`, `ci`, `python`, `ai`, `mcp`, `tools`

## Context

The fork ships eight independent Python distributions, each with its own
`pyproject.toml`, `tests/` directory, and `requires-python` range:

| Package | Path | Python range |
|---|---|---|
| Legacy harness | `python/` | 3.11 (tox-driven) |
| Tiny-AI training | `ai/` | 3.11 – 3.14 |
| MCP server | `mcp-server/vmaf-mcp/` | 3.10+ |
| vmaf-tune | `tools/vmaf-tune/` | 3.10 – 3.14 |
| vmaf-roi-score | `tools/vmaf-roi-score/` | 3.10 – 3.12 |
| ensemble-training-kit | `tools/ensemble-training-kit/` | 3.12 |
| dev-llm | `dev-llm/` | 3.11+ |
| Root meta | (root `pyproject.toml`) | 3.14.5+ |

CI invokes each package's pytest separately via ad-hoc `python3 -m venv
... && pip install -e .[dev] && pytest tests/` recipes in
`.github/workflows/tests-and-quality-gates.yml` (lines 349 – 460 for
ai/MCP/vmaf-tune). The recipes drift in lockstep with each
`pyproject.toml` change, and a developer who wants to reproduce a
specific CI lane locally has to copy-paste from the YAML.

Only `python/` has an orchestrator (`python/tox.ini`, used by
`tox -c python`). It owns the Netflix golden-data gate plus the
Cython build of `compat/python-vmaf/core/adm_dwt2_cy`, so it is not
portable to the other packages without bringing along the C build chain.

The pain is local-developer ergonomics, not CI correctness — CI already
works. Without an entry point a developer has to remember the per-package
venv recipe; a unified `nox -s ai` / `nox -s mcp` etc. shortens the loop
to one command per suite.

## Decision

Add `noxfile.py` at the repository root with one session per Python
package (`ai`, `mcp`, `vmaf_tune`, `dev_llm`, `roi_score`,
`ensemble_kit`, `python_harness`) plus two meta-sessions (`all`, `lint`).
Each session creates a throw-away venv, installs the package via
`pip install -e <path>[dev]`, and runs `pytest <path>/tests/`. The
`python_harness` session shells out to `tox -c python` rather than
re-implementing the Cython + golden-data setup.

CI keeps its existing per-package pytest invocations untouched. Nox is a
**local-developer affordance**, not a CI gate. The CI gate is the
ad-hoc venv recipes that already match the per-package CI matrix.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **nox at repo root** *(chosen)* | Pythonic, no `setup.py` needed, each session is plain Python, easy to read/edit, used by Pallets/scientific-python projects | One more dev dep (nox itself) | Wins on every axis except adding 1 dep |
| Extend `python/tox.ini` to cover every package | Re-uses existing tooling | tox config is INI-based + harder to compose across N packages with different Python pins; `usedevelop` model collides with `pyproject.toml`-only packages; mixing the Cython golden-data env with pure-Python suites is fragile | Configuration density goes up faster than the package count |
| `Makefile` targets per package (`make ai-test`, `make mcp-test`, …) | No new dep, fits existing `make lint` / `make format` muscle memory | No isolation — each target runs in the developer's ambient Python and pollutes site-packages with editable installs; reproducing CI venv layout requires reimplementing `pip install` recipes inside the Makefile | Loses the "throw-away venv per session" guarantee the CI lanes rely on |
| Status quo (per-package recipes in CI YAML only) | Zero new files | Developer ergonomics stay bad; copy-paste from CI YAML is error-prone; recipe drift goes undetected locally until CI fails | The user explicitly asked for a unified entry point in this audit |
| pytest workspace plugin (e.g. `pytest-xdist` with `--rootdir` per package) | Single `pytest` invocation runs all suites | Cross-package dependency isolation breaks: `ai/` needs `torch`, `mcp/` needs `mcp`, `vmaf-tune/` needs `optuna` — co-resolving them in one venv produces conflicts | Hard fail at the dependency-resolver step |

## Consequences

- **Positive**:
  - Developers run `nox -s ai` (or `mcp`, `vmaf_tune`, …) without
    remembering each package's venv recipe.
  - `nox -l` is the discoverable index of every Python suite.
  - The orchestrator is single-source-of-truth for the dev experience;
    CI keeps its independent recipes for matrix reasons.
- **Negative**:
  - One additional dev dependency (`nox`) to install locally. Not a
    CI dep — CI does not call nox.
  - When a new Python package gets added to the repo, the contributor
    has to add a session to `noxfile.py` AND a job to
    `tests-and-quality-gates.yml`. Documented in
    `docs/development/python-test-orchestrator.md`.
- **Neutral / follow-ups**:
  - The `python_harness` session intentionally delegates to tox rather
    than duplicating the Cython + golden-data setup. If tox is ever
    removed from `python/`, the delegation flips to a direct invocation
    of the build steps.
  - The `lint` session mirrors `make lint-py` but uses pinned tools in
    a controlled venv. The Makefile target stays the canonical lint
    entry point for the CI gate.

## References

- Source: `req` (user direction to audit Python test orchestration and
  add a unified entry point if missing).
- Related: `python/tox.ini` (legacy harness, kept), [ADR-0042](0042-tinyai-docs-required-per-pr.md)
  (tiny-AI docs requirement, satisfied by
  `docs/development/python-test-orchestrator.md`),
  [ADR-0100](0100-project-wide-doc-substance-rule.md) (per-surface doc
  bar), [ADR-0108](0108-deep-dive-deliverables-rule.md) (six
  deliverables).
