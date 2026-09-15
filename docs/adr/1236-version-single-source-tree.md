<!-- markdownlint-disable MD013 MD024 MD033 MD041 MD060 -->
# ADR-1236: Single-source package versions and unify Python dependencies

- **Status**: Accepted
- **Date**: 2026-09-08
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: build, ci, python, packaging, renovate

## Context

Prior to this decision, package versions and dependencies across the repository suffered from two forms of fragmentation:

1. **Python Dependency Triplication**: The runtime dependency list for the `vmaf` package was declared in three separate places:
   - `python/pyproject.toml` under `[project].dependencies`
   - `python/requirements.txt`
   - `python/setup.py` under `install_requires=[...]`

   Renovate evaluates distinct package managers independently (`pep621` for `pyproject.toml`, `pip_requirements` for `requirements.txt`, and `pip_setup` for `setup.py`). As a result, every single dependency update opened two competing, unmergeable PRs (such as #1409 touching `requirements.txt` and #1410 touching `pyproject.toml` and `setup.py`).

2. **Cross-Tree Version Disagreements**: A repository-wide audit revealed 25 packages declared in more than one file, with 5 instances of version skew:
   - `numpy`: 10 sites with disparate versions (`1.26.0`, `2.0.0`, `2.3.3`, `2.5.2`, and unpinned in `[build-system]`).
   - `scipy`: 8 sites drifting between `1.14.1` and `1.18.1`.
   - `matplotlib`: 4 sites drifting between `3.9.3` and `3.11.1`.
   - `pyarrow`: 3 sites drifting between `13.0.0` and `25.0.1`.
   - Package version manifests: `tools/vmaf-tune/pyproject.toml` declared `0.0.1` while `src/vmaftune/__init__.py` declared `0.0.2`; and `build-config.env` lacked an authoritative `VMAFX_VERSION` knob.

## Decision

1. **`python/pyproject.toml [project].dependencies` is the single owner of python package dependencies**:
   - `python/setup.py` removes the duplicate `install_requires=[...]` definition. Modern setuptools natively loads `[project].dependencies` from `pyproject.toml`.
   - `python/requirements.txt` is derived mechanically from `python/pyproject.toml` via `scripts/ci/check-python-requirements-single-source.sh --write` (wrapped as `make python-deps-sync`).
   - `scripts/ci/check-python-requirements-single-source.sh` runs as a CI gate and pre-commit hook to compare the ordered dependency entries (comments and blank lines are ignored).
   - `renovate.json` adds `python/requirements.txt` to `ignorePaths`. Renovate updates only `python/pyproject.toml`, eliminating duplicate and competing PRs.

2. **Resolution of Version Disagreements (Newest-Version Rule)**:
   Per explicit policy, all divergent pins are resolved to the newest version rather than the lowest common denominator:
   - The `vmaf` NumPy build floor follows its current runtime floor, `>=2.5.3`; rebasing preserves the already merged NumPy and PyWavelets updates.
   - `scipy` is unified to `>=1.18.1`.
   - `matplotlib` is unified to `>=3.11.1`.
   - `pyarrow` is unified to `>=25.0.1`.
   - `tools/vmaf-tune/pyproject.toml` is bumped to `0.0.2` to match its implementation.
   - `optuna` in `tools/vmaf-tune/pyproject.toml [dev]` is aligned to `>=5.0.0`.
   - `mcp-server/vmaf-mcp/pyproject.toml` replaces `hatchling==1.32.0` with `hatchling>=1.32.0`.

3. **Extend `build-config.env` as the global version single-source**:
   `build-config.env` is augmented with:
   - `VMAFX_VERSION="3.2.1"` (tracked in `release-please-config.json` `extra-files`)
   Scientific Python floors remain owned by each package manifest. The five initially proposed scientific-stack globals had no consumers or drift checks and were removed during review. Native ORT archive roles remain separate from Python dependency floors; see the [ownership guide](../development/base-images.md#python-and-onnx-runtime-ownership).
   `scripts/ci/check-workflow-versions.py` is updated to validate `PYTHON_CI_VERSION` and `VMAFX_VERSION` against the workflows and core manifests.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **`pyproject.toml` single owner + generated `requirements.txt` + gate** (chosen) | Eliminates Renovate duplicates; retains compatibility with `pip -r` workflows and Docker builds; prevents silent drift | `requirements.txt` is checked into git | Standard repository pattern (matches `build-config.env` / `base-images-sync`) |
| **Delete `requirements.txt` completely** | Zero duplication | Breaks upstream Netflix documentation, Dockerfile recipes, and legacy `pip install -r` consumers | Unnecessary breakage when mechanical derivation solves the drift |
| **Keep `requirements.txt` as primary and generate `pyproject.toml`** | Legacy familiarity | Violates PEP 621 standards; setuptools and modern build backends expect `pyproject.toml` | `pyproject.toml` is the standard Python packaging metadata authority |
| **Lowest-common-denominator version alignment** | Minimizes risk of upstream compatibility issues | Stagnates dependency versions; runs counter to explicit user requirement for current versions | User explicitly mandated newest version resolution |

## Consequences

- **Positive**:
  - Renovate no longer manages the generated `vmaf` requirements list separately from its package metadata.
  - The ordered requirements list is checked against package metadata locally and in CI; `setup.py` no longer duplicates it.
  - The changed package metadata and workflow mirrors have explicit owners; this is not a claim that every historical or compatibility pin has been unified.
- **Negative**:
  - Developers editing `python/pyproject.toml` dependencies must run `make python-deps-sync` before committing (enforced by pre-commit hook).
- **Neutral / follow-ups**:
  - `release-please` automatically updates `build-config.env` on version cuts.

## References

- Follows the authority-plus-drift-check model of [ADR-1231](1231-base-image-single-source.md).
- Python package metadata standards: PEP 517, PEP 518, PEP 621.
- Refs user prompt requirement on branch `build/version-single-source-tree`.

- `req` (2026-09-08): “everything that needs to be configured in mutliple places is in global envs”. A declaration becomes an owner only when consumers or executable drift checks use it.
