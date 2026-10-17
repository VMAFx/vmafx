<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3335: Python package floors match dependency resolutions

- **Status**: Accepted
- **Date**: 2026-10-16
- **Deciders**: Lusoris
- **Tags**: python, build, packaging

## Context

Several Python packages in the repository declared `requires-python` floors of
`>=3.10` or `>=3.11` (`python/`, `ai`, `dev-llm`, `mcp-server/vmaf-mcp`,
`tools/vmaf-roi-score`, `tools/vmaf-tune`), while foundational dependencies
in their core or optional extra sets require higher versions. In particular,
`numpy>=2.5.3` requires Python >=3.12, and `matplotlib>=3.11.2` requires
Python >=3.11.

Attempting to resolve or install these packages on Python 3.10 or 3.11 with
modern installers like `uv pip compile` or `pip install` failed at dependency
resolution because declared core or extra dependencies could not be satisfied.
The manifests claimed support for interpreter versions on which the packages
could not be installed.

## Decision

We set each package's `requires-python` floor to the lowest interpreter version
where core dependencies and all optional extra groups resolve successfully.
Tool targets follow the declared floor in each changed package:

- `[tool.ruff] target-version` is updated to match (`py311` -> `py312` for `ai`
  and `dev-llm`, `py310` -> `py312` for `mcp-server/vmaf-mcp`).
- Upper version bounds (such as `<3.15`) are preserved.
- The root `pyproject.toml` remains at `>=3.14`, governing repository-level
  linting and developer tooling ([ADR-1282](1282-mypy-python-version-follows-requires-python.md)).

We enforce this policy through `scripts/ci/check_python_floors.py` (Makefile
target `make python-floors-check`), which compiles each tracked package with all
extras in an isolated temporary environment at its declared floor. The script
fails closed on invalid specifications, unresolvable floors, or missing tools,
and provides a `--tight` flag to verify that no lower minor version resolves.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| (a) Lowest resolvable floor per package + check | Accurate per-package declarations; preserves oldest viable interpreter per package; automated check prevents drift. | Uneven floors across packages (some 3.11, some 3.12). | **Chosen**. Provides honest metadata while maximising interpreter compatibility where dependencies allow. |
| (b) Raise only where core fails, extras documented | Base package installable on older Python when core allows it. | `pip install pkg[eval]` or `pkg[dev]` fails at resolve on 3.11; misleading `requires-python` in metadata. | Rejected. Users expect declared extras to resolve on supported Python floors. |
| (c) Every package to 3.14 | Uniform across the repository; matches CI runner environment. | Drops support for Ubuntu 24.04 LTS and Debian 13 system Pythons for standalone packages. | Rejected. Artificially restricts deployment platforms where dependencies resolve on 3.11 or 3.12. |

## Consequences

- **Positive**: Package manifests honestly reflect dependency requirements. Users
  and downstream packagers avoid resolution failures when installing packages
  with any extra group. `make python-floors-check` prevents regression.
- **Negative**: Environments on Python 3.10 or 3.11 cannot install packages whose
  direct dependencies demand Python 3.12.
- **Neutral / follow-ups**: No runtime test below Python 3.14 currently executes
  in CI workflows — declared floors are resolve-proven via `uv pip compile`, not
  run-proven across the test matrix (named gap).

## References

- Controller ledger `vmafx-requires-python-floors` (paraphrased): floor per
  package where core and every extra resolve, kept by a check with a planted red
  case.
- [ADR-1282](1282-mypy-python-version-follows-requires-python.md) for root tooling baseline.
- [ADR-1305](1305-hash-locked-python-installs.md) for hash-locked dependency inputs.
