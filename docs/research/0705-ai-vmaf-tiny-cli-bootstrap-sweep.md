# Research 0705: VMAF Tiny Script Bootstrap Sweep

## Question

Which versioned `vmaf_tiny` training/export/validation scripts still use local
path setup, direct parser construction, or `sys.argv[1:]` provenance capture
after the shared AI script helpers landed?

## Inputs Reviewed

- `ai/scripts/train_vmaf_tiny_v2.py`
- `ai/scripts/train_vmaf_tiny_v3.py`
- `ai/scripts/train_vmaf_tiny_v4.py`
- `ai/scripts/train_vmaf_tiny_v5.py`
- `ai/scripts/export_vmaf_tiny_v2.py`
- `ai/scripts/export_vmaf_tiny_v3.py`
- `ai/scripts/export_vmaf_tiny_v4.py`
- `ai/scripts/validate_vmaf_tiny_v2.py`
- `ai/scripts/validate_vmaf_tiny_v3.py`
- `ai/scripts/validate_vmaf_tiny_v4.py`
- `ai/tests/test_vmaf_tiny_train_run_provenance.py`
- `ai/tests/test_vmaf_tiny_export_run_provenance.py`
- `ai/tests/test_vmaf_tiny_validator_reports.py`

## Findings

The active versioned tiny-model scripts were already writing run provenance,
but they used three slightly different setup patterns:

- train scripts manually derived `SCRIPT_PATH`, `REPO_ROOT`, and inserted
  `ai/src` into `sys.path`;
- export scripts relied on the caller environment to make `aiutils`
  importable;
- validator scripts inserted `ai/src` directly and separately collected
  `sys.argv[1:]`.
- downstream eval harnesses import the train scripts through the
  `ai.scripts.*` package path, so the bootstrap import must also work when
  `ai/scripts` itself is not already on `sys.path`.

Those differences are not model behavior. They increase maintenance cost and
make direct invocation less predictable across host shells, tests, and the
dev-MCP container.

## Scope Chosen

Migrate the versioned train/export/validate group to:

- `bootstrap_ai_script(__file__)`;
- `make_argument_parser(...)`;
- `collect_cli_argv(argv)`;
- bootstrap-provided `SCRIPT_PATH` / `REPO_ROOT` values for
  `run_provenance`.

The bootstrap import uses a direct-script path first and a package-path
fallback second, preserving both `python ai/scripts/foo.py` and
`from ai.scripts import foo` call sites.

## Reproducer / Smoke

```bash
.venv/bin/python -m pytest \
  ai/tests/test_vmaf_tiny_train_run_provenance.py \
  ai/tests/test_vmaf_tiny_export_run_provenance.py \
  ai/tests/test_vmaf_tiny_validator_reports.py \
  ai/tests/test_eval_report_run_provenance.py -q
```

## Limits

This is a behavior-preserving script hygiene sweep. It does not retrain,
re-export, or promote any ONNX checkpoint.
