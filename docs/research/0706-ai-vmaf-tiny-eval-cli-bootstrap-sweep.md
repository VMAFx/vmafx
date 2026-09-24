# Research 0706: VMAF Tiny Eval Script Bootstrap Sweep

## Question

Which VMAF tiny evaluation scripts still carry local path setup, direct parser
construction, or raw `sys.argv` provenance capture after the shared AI helpers
landed?

## Inputs Reviewed

- `ai/scripts/eval_loso_vmaf_tiny_v3.py`
- `ai/scripts/eval_loso_vmaf_tiny_v4.py`
- `ai/scripts/eval_loso_vmaf_tiny_v5.py`
- `ai/scripts/eval_multiseed_v3_v4.py`
- `ai/scripts/eval_loso_mlp_small.py`
- `ai/scripts/eval_loso_3arch.py`
- `ai/tests/test_eval_report_run_provenance.py`
- `ai/tests/test_legacy_eval_report_run_provenance.py`

## Findings

The LOSO and multi-seed eval harnesses already emitted run provenance, but
they still used a mix of direct `sys.path.insert(...)` setup, bare
`argparse.ArgumentParser`, and direct `sys.argv` capture. That made the eval
scripts behave differently from the train/export/validate scripts and made the
report provenance less uniform across old and current harnesses.

## Scope Chosen

Move the eval harnesses to the accepted helper pattern:

- bootstrap imports through `bootstrap_ai_script(__file__)`;
- parse with `make_argument_parser(...)`;
- collect replay argv with `collect_cli_argv(argv)`;
- write `run_provenance["argv"]` from the normalized replay argv.

The direct-script bootstrap keeps both invocation shapes working:
`python ai/scripts/foo.py ...` and `from ai.scripts import foo`. Scripts that
import `ai.scripts.*` or `ai.train.*` pass `include_repo_root=True`; the
bootstrap's default adds `ai/src`, not the repository root.

## Reproducer / Smoke

```bash
.venv/bin/python -m pytest \
  ai/tests/test_eval_report_run_provenance.py \
  ai/tests/test_legacy_eval_report_run_provenance.py -q
```

## Limits

This is a behavior-preserving CLI/provenance hygiene sweep. It does not run a
new LOSO job, retrain any model, or update shipped ONNX artefacts.

## 2026-09-24 restoration note

Commit `d170ef86a` silently restored the pre-sweep bodies while leaving this
digest, ADR-0680/0681, the AGENTS invariant, and the changelog fragments in
place. BUG-048 restores the six current scripts and adds an AST regression that
checks the helper calls semantically instead of matching formatted source text.
Direct `--help` probes exposed and corrected the missing `include_repo_root`
flag on the five scripts that import the repository package.
