# ADR-0879: Python dependency freshness sweep (2026-05-30)

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: Lusoris
- **Tags**: security, ai, mcp, deps, python, fork-local

## Context

The fork ships eight `pyproject.toml` files (`ai/`, `mcp-server/vmaf-mcp/`,
`dev-llm/`, `tools/vmaf-tune/`, `tools/vmaf-roi-score/`,
`tools/ensemble-training-kit/`, plus the two harness shims at the repo root
and under `python/`) and four `requirements*.txt` files. Their floor pins
drift slowly relative to PyPI: a package that was at-latest when a tree was
authored is rarely at-latest a few weeks later, and the fork's
deliverables-bar (`ADR-0108`) does not require a periodic refresh on its own.

This sweep audits every fork-local Python dep file against PyPI's current
`info.version` and bumps the floors that are behind the latest published
release. The goal is *floor freshness*, not exact pinning — consumers who
want pristine determinism use the dedicated `requirements-frozen.txt`
artefact already shipped under `tools/ensemble-training-kit/` (ADR-0324).

Findings:

- Most floors were already at-latest (numpy, pandas, scipy, torch,
  onnxruntime, transformers, etc. — the 2026-vintage tree mostly tracks
  PyPI within hours of release).
- A small cluster of dev / cloud / agent-SDK deps had drifted one minor or
  patch behind: `typer` (0.25.1 → 0.26.4), `ruff` (0.15.14 → 0.15.15),
  `anthropic` (0.104.1 → 0.105.2), `openai` (2.37.0 → 2.38.0),
  `pytorch-lightning` (2.6.4 → 2.6.5), `mcp` (1.27.1 → 1.27.2),
  `pytest-asyncio` (1.3.0 → 1.4.0), `pandas-stubs` (3.0.0.260204 →
  3.0.3.260530), `pytest-cov` (unpinned → ≥7.1.0).
- `tools/vmaf-tune/pyproject.toml` carried a *major*-version stale floor on
  the dev extra — `optuna>=3.6` while the `fast` extra (the actual user-
  facing path) already requires `optuna>=4.8.0`. Aligned the dev extra to
  the same 4.x floor so dev installs match the runtime path.
- No `requirements*.txt` file currently uses `--hash=sha256:` pinning. The
  audit considered switching to `pip-compile --generate-hashes` but defers
  that to a follow-up because the immediate value (floor-freshness against
  PyPI today) is independent of the hash-pinning question and the latter
  warrants its own deliverables cycle (lockfile placement, CI gate,
  refresh cadence, ADR — none of which fit a single drive-by sweep).

## Decision

Bump the eight pin sites identified above to their PyPI-latest floors as
of 2026-05-30. Defer hash-pinning to a separate ADR + PR. Do not change
any major-version constraint ceilings (`<3.0`, `<2.0`, etc.) — those
encode known incompatibilities and are not part of this freshness sweep.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Bump floors + add `--hash=sha256:` to every `requirements*.txt` in one PR | Single security touch | Cross-cuts lockfile policy (cadence, CI gate, refresh tooling); turns a 9-line bump into a ~600-line lockfile-and-process PR | Hash pinning is a policy decision worth its own ADR cycle |
| Switch all `>=` floors to `==` exact pins | Bit-reproducible installs | Pyproject `==` pins fight the user's local environment + conflict with sibling tools; the fork already has `requirements-frozen.txt` for that need | Exact pins are a deployment concern, not a library concern |
| Skip the sweep, let Renovate file PRs piecemeal | Lower-touch | Renovate already runs but the fork's many small `pyproject.toml` files generate noisy individual PRs; a periodic sweep batches them | Renovate stays valuable for between-sweep coverage; the sweep complements rather than replaces it |
| Add ceiling bumps too (`<3.0` → `<4.0`) | Forward-compat | Ceilings encode tested compatibility; bumping a ceiling without testing the new major is the actual security risk | Ceiling moves require validation runs, out of scope here |

## Consequences

- **Positive**: Eight floors now match PyPI latest. Dev installs of
  `tools/vmaf-tune` pull a contemporary Optuna instead of a 2-year-old
  3.6.x. The `dev-llm` cloud extra picks up the current Anthropic / OpenAI
  SDKs (1 minor release of bug fixes each).
- **Negative**: Anyone with a `pip install -e .[dev]` cache from the last
  ~week will see a fresh wheel resolve. No API breaks expected at this
  granularity.
- **Neutral / follow-ups**: Hash-pinning lockfiles for the three install-
  facing requirements files (`python/requirements.txt`,
  `docs/requirements.txt`, `tools/ensemble-training-kit/requirements-frozen.txt`)
  deserve a dedicated ADR + PR.

## References

- ADR-0108: deep-dive deliverables rule (this sweep ships the full six).
- ADR-0221: changelog fragments under `changelog.d/`.
- ADR-0324: ensemble-training-kit frozen requirements (the prior art for
  hash-style exact pinning in the fork).
- Audit dossier: `docs/research/python-dep-freshness-2026-05-30.md`.
- Source: agent task brief (paraphrased: audit Python `pyproject.toml`
  files and `requirements*.txt` for outdated deps + missing hash pinning).
