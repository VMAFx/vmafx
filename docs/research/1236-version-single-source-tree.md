<!-- markdownlint-disable MD013 MD022 MD024 MD032 MD033 MD041 MD060 -->
# Research Digest: Version Single-Source Tree & Python Dependency Unification

- **Author**: Lusoris, Claude (Anthropic)
- **Date**: 2026-09-08
- **Topic**: Single-sourcing package versions across manifests and eliminating Python dependency triplication
- **Related ADR**: [ADR-1236](../adr/1236-version-single-source-tree.md)

## 1. Problem Inventory

The counts below describe the original pre-rebase inventory. The reviewed branch preserves newer base dependency updates; package-runtime acceptance is separate from metadata validation.

Prior to this work, versions across the codebase were declared across heterogeneous manifests, resulting in drift, maintenance drag, and competing dependency automation PRs.

### 1.1 Python Dependency Triplication

The Python dependency list for the `vmaf` package was declared identically across three files:

1. `python/pyproject.toml` (`[project].dependencies`)
2. `python/requirements.txt`
3. `python/setup.py` (`install_requires=[...]`)

Because Renovate evaluates multiple managers independently (`pep621` for `pyproject.toml`, `pip_requirements` for `requirements.txt`, and `pip_setup` for `setup.py`), a single upstream dependency release (e.g. `PyWavelets` >=1.9.0 → >=1.10.0) generated two competing pull requests (#1409 targeting `requirements.txt` and #1410 targeting `pyproject.toml` + `setup.py`). Furthermore, `setup.py` duplicated the dependency list manually, despite modern `setuptools` natively reading `[project].dependencies` from `pyproject.toml`.

### 1.2 Divergent Package Declarations

A repository scan identified 25 packages declared across multiple files, with 5 versions/floors that actively disagreed or drifted:

- **`numpy`**: Present in 10 sites across the tree. Disagreements ranged from historical floors (`1.26.0`, `2.0.0`, `2.3.3`) up to `2.5.2`. In `python/pyproject.toml [build-system]`, `numpy` was unpinned despite runtime requiring `>=2.5.2`.
- **`scipy`**: Present in 8 sites, drifting between `1.14.1` and `1.18.1`.
- **`matplotlib`**: Present in 4 sites, drifting between `3.9.3` and `3.11.1`.
- **`pyarrow`**: Present in 3 sites, drifting between `13.0.0` and `25.0.1`.
- **Package `version` fields**:
  - `tools/vmaf-tune/pyproject.toml` declared `0.0.1`, while its implementation `src/vmaftune/__init__.py` declared `__version__ = "0.0.2"`.
  - `tools/vmaf-tune/pyproject.toml` also declared `optuna>=4.9.0` in `[dev]` while `[fast]` and `ai/pyproject.toml` required `optuna>=5.0.0`.
  - `mcp-server/vmaf-mcp/pyproject.toml` pinned `hatchling==1.32.0` in `[build-system]` while other manifests allowed `>=1.32.0`.
  - `build-config.env` lacked a release version knob corresponding to `VMAFX_VERSION`.

## 2. Resolution Strategy & Verification

Following the explicit project resolution rule ("take the newest version in every case; never downgrade to make things agree"):

| Package | Old Sites / Divergent Values | New Single Source | Chosen Version | Rationale |
|---|---|---|---|---|
| `numpy` | 10 sites: 1.26.0 / 2.0.0 / 2.3.3 / 2.5.2 / unpinned | `python/pyproject.toml` (`[build-system]` & `[project].dependencies`), `ai/pyproject.toml`, `mcp-server/` | `numpy>=2.5.3` for the current `vmaf` build/runtime metadata | Preserves the newer base floor; no fresh scientific runtime compatibility claim |
| `scipy` | 8 sites: 1.14.1 / 1.18.1 | `python/pyproject.toml`, `ai/pyproject.toml`, `mcp-server/` | `scipy>=1.18.1` | Modern scientific Python stack requirement on Python 3.14 |
| `matplotlib` | 4 sites: 3.9.3 / 3.11.1 | `python/pyproject.toml`, `ai/pyproject.toml [viz]` | `matplotlib>=3.11.1` | Matches visualization stack |
| `pyarrow` | 3 sites: 13.0.0 / 25.0.1 | `ai/pyproject.toml`, `mcp-server/` | `pyarrow>=25.0.1` | Matches dataset and parquet evaluators |
| `vmaf-tune` (version) | `pyproject.toml` (0.0.1) vs `__init__.py` (0.0.2) | `tools/vmaf-tune/pyproject.toml` & `src/vmaftune/__init__.py` | `0.0.2` | Bumped manifest to match implementation |
| `optuna` | `vmaf-tune` dev (4.9.0) vs fast (5.0.0) & `ai/` (5.0.0) | `tools/vmaf-tune/pyproject.toml` | `optuna>=5.0.0` | Unified to Optuna v5 |
| `hatchling` | `mcp-server` (==1.32.0) vs others | `mcp-server/vmaf-mcp/pyproject.toml` | `>=1.32.0` | Avoid exact pin conflict |
| `vmafx` (product) | `core/meson.build`, `compat/python-vmaf/`, `ai/`, `dev-llm/`, `mcp-server/`, `Chart.yaml` | `build-config.env` (`VMAFX_VERSION="3.2.1"`) | `3.2.1` | Added to `build-config.env` and tracked in `release-please-config.json` |

## 3. Single Ownership Architecture

1. **Python Package Dependencies**:
   `python/pyproject.toml [project].dependencies` is established as the single owner.
   - `python/setup.py` drops `install_requires=[...]`; modern `setuptools` natively loads `dependencies` from `pyproject.toml`.
   - `python/requirements.txt` is derived directly from `python/pyproject.toml` via `scripts/ci/check-python-requirements-single-source.sh --write` (`make python-deps-sync`).
   - `scripts/ci/check-python-requirements-single-source.sh` acts as a CI gate and pre-commit hook comparing ordered dependency entries while ignoring comments and blank lines.
   - `renovate.json` ignores `python/requirements.txt` (`ignorePaths`), eliminating duplicate and competing PRs.

2. **`build-config.env` consumption**:
   `VMAFX_VERSION` is release-owned and checked against core/Python version markers;
   `PYTHON_CI_VERSION` is checked against workflow mirrors. The existing Level Zero
   container-consumption check remains active after the workflow checker refactor.
   Five proposed scientific-stack globals had no consumers or drift checks and
   were removed during review. Their package manifests remain the owners.

3. **Native ORT roles**:
   CPU archives, CUDA compatibility coverage and Python dependency floors are
   distinct contracts. The [ownership guide](../development/base-images.md#python-and-onnx-runtime-ownership)
   records those boundaries. A version bump must validate the actual asset name
   and runtime-library requirements before consolidating a lane.
