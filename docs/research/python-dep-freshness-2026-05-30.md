# Python dependency freshness audit — 2026-05-30

**Scope**: every fork-local `pyproject.toml` and `requirements*.txt`
checked against PyPI's current `info.version`. Master tip
`387839eacf` at audit time.

**Method**:

```bash
# Enumerate
find . -type f \( -name 'pyproject.toml' -o -name 'requirements*.txt' \
  -o -name 'Pipfile' -o -name 'poetry.lock' \) \
  -not -path './build*' -not -path './core/build*'

# For each pinned package
curl -s "https://pypi.org/pypi/<pkg>/json" \
  | python3 -c "import sys,json; print(json.load(sys.stdin)['info']['version'])"
```

**Files audited (12 total)**:

- `pyproject.toml` (root — empty)
- `python/pyproject.toml` (harness shim — empty)
- `dev-llm/pyproject.toml`
- `tools/vmaf-tune/pyproject.toml`
- `tools/vmaf-roi-score/pyproject.toml`
- `tools/ensemble-training-kit/pyproject.toml`
- `ai/pyproject.toml`
- `mcp-server/vmaf-mcp/pyproject.toml`
- `python/requirements.txt`
- `python/test/requirements.txt`
- `docs/requirements.txt`
- `tools/ensemble-training-kit/requirements-frozen.txt`

## Findings

### At-latest (no action)

| Package | Pinned floor | PyPI latest | Site(s) |
| --- | --- | --- | --- |
| `torch` | `>=2.12.0` | 2.12.0 | ai, mcp, ensemble |
| `onnx` | `>=1.21.0` | 1.21.0 | ai, mcp, ensemble |
| `onnxruntime` | `>=1.26.0` | 1.26.0 | ai, mcp, ensemble |
| `numpy` | `>=2.4.6` | 2.4.6 | ai, mcp, python, ensemble |
| `pandas` | `>=3.0.3` | 3.0.3 | ai, mcp, python, ensemble |
| `scipy` | `>=1.17.1` | 1.17.1 | ai, mcp, python, ensemble |
| `scikit-learn` | `>=1.8.0` | 1.8.0 | ai, python, ensemble |
| `pyyaml` | `>=6.0.3` | 6.0.3 | ai, mcp, dev-llm, ensemble |
| `onnxscript` | `>=0.7.0` | 0.7.0 | ai |
| `pyarrow` | `>=24.0.0` | 24.0.0 | ai, mcp |
| `Pillow` | `>=12.2.0` | 12.2.0 | ai, mcp |
| `tqdm` | `>=4.67.3` | 4.67.3 | ai |
| `pytest` | `>=9.0.3` | 9.0.3 | ai, mcp, dev-llm, vmaf-tune, vmaf-roi-score |
| `mypy` | `>=2.1.0` | 2.1.0 | ai, mcp, dev-llm |
| `pydantic` | `>=2.13.4` | 2.13.4 | mcp |
| `anyio` | `>=4.13.0` | 4.13.0 | mcp |
| `transformers` | `>=5.9.0` | 5.9.0 | mcp[vlm] |
| `accelerate` | `>=1.13.0` | 1.13.0 | mcp[vlm] |
| `rich` | `>=15.0.0` | 15.0.0 | ai, dev-llm |
| `optuna` | `>=4.8.0` | 4.8.0 | ai[tune], vmaf-tune[fast] |
| `ray` | `>=2.55.1` | 2.55.1 | ai[tune] |
| `matplotlib` | `>=3.10.9` | 3.10.9 | ai[viz], python |
| `seaborn` | `>=0.13.2` | 0.13.2 | ai[viz] |
| `scikit-image` | `>=0.26.0` | 0.26.0 | python |
| `h5py` | `>=3.16.0` | 3.16.0 | python |
| `dill` | `>=0.4.1` | 0.4.1 | python |
| `PyWavelets` | `>=1.9.0` | 1.9.0 | python |
| `python-slugify` | `>=8.0.4` | 8.0.4 | python |
| `sureal` | `>=0.9.0` | 0.9.0 | python |
| `libsvm-official` | `>=3.37.0,<=3.37` | 3.37.0 | python |
| `mkdocs` | `>=1.6.1,<2` | 1.6.1 | docs |
| `mkdocs-material` | `>=9.7.6,<10` | 9.7.6 | docs |
| `mkdocs-minify-plugin` | `>=0.8` | 0.8.0 | docs |
| `types-PyYAML` | `>=6.0.12.20260518` | 6.0.12.20260518 | ai[dev] |

### Bumped (this PR)

| Package | Old floor | New floor | Severity | Site(s) |
| --- | --- | --- | --- | --- |
| `optuna` (dev extra) | `>=3.6` | `>=4.8.0` | HIGH (major) | tools/vmaf-tune[dev] |
| `typer` | `>=0.25.1` | `>=0.26.4` | MEDIUM (minor) | ai, dev-llm |
| `anthropic` | `>=0.104.1` | `>=0.105.2` | MEDIUM (minor) | dev-llm[cloud] |
| `openai` | `>=2.37.0` | `>=2.38.0` | MEDIUM (minor) | dev-llm[cloud] |
| `pytest-asyncio` | `>=1.3.0` | `>=1.4.0` | MEDIUM (minor) | mcp[dev] |
| `pytorch-lightning` | `>=2.6.4` | `>=2.6.5` | LOW (patch) | ai |
| `mcp` | `>=1.27.1` | `>=1.27.2` | LOW (patch) | mcp |
| `ruff` | `>=0.15.14` | `>=0.15.15` | LOW (patch) | ai, dev-llm, mcp, vmaf-tune, vmaf-roi-score |
| `pandas-stubs` | `>=3.0.0.260204` | `>=3.0.3.260530` | LOW (datestamp) | ai[dev] |
| `pytest-cov` | (unpinned) | `>=7.1.0` | LOW (floor add) | python/test |

The `optuna` bump is the one with material substance: `tools/vmaf-tune`'s
`fast` extra already required `optuna>=4.8.0`, so the dev extra's
`optuna>=3.6` floor was internally inconsistent — pip would happily install
3.x for `pip install -e .[dev]` while the runtime code path under
`fast` needs 4.x. Realigning to 4.8.0 prevents that mismatch.

## Hash pinning (deferred)

None of the four `requirements*.txt` files use `--hash=sha256:` pinning. The
two install-facing ones (`python/requirements.txt`,
`docs/requirements.txt`) and the kit's `requirements-frozen.txt` are the
candidates for `pip-compile --generate-hashes`. Deferred because:

1. Hash pinning needs a policy ADR: where lockfiles live, how often they
   refresh, which CI gate enforces them, what the upgrade flow is.
2. The freshness sweep is independent — adding hashes today against
   already-fresh floors is no harder tomorrow.
3. A 600-line lockfile PR would dwarf the 9-line bump PR; reviewability
   suffers.

Follow-up: file a separate planning entry once the lockfile-policy ADR is
drafted.

## Reproducer

```bash
# Re-run the audit
python3 -c "
import json, urllib.request
pkgs = ['typer','ruff','anthropic','openai','pytorch-lightning','mcp',
        'pytest-asyncio','pandas-stubs','optuna']
for p in pkgs:
    with urllib.request.urlopen(f'https://pypi.org/pypi/{p}/json') as r:
        print(p, '=', json.load(r)['info']['version'])
"

# Validate TOML after edits
python3 -c "
import tomllib
for f in ['ai/pyproject.toml','mcp-server/vmaf-mcp/pyproject.toml',
         'dev-llm/pyproject.toml','tools/vmaf-tune/pyproject.toml',
         'tools/vmaf-roi-score/pyproject.toml']:
    tomllib.load(open(f,'rb')); print('OK', f)
"
```
