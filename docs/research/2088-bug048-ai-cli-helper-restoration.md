# Research-2088: BUG-048 AI CLI Helper Restoration

## Question

Did the accepted AI bootstrap and CLI-helper pattern survive in the twelve
legacy evaluation, quantization, and export scripts whose documentation still
claimed it was present?

## Historical and current evidence

- `d02922fc2` migrated the six eval scripts under Research-0706.
- `cc4ea5014` migrated the six quant/export scripts under Research-0707.
- `d170ef86a` changed all twelve files back to hand-written path, parser, and
  `sys.argv` handling while retaining ADR-0680, ADR-0681, both research
  digests, both changelog fragments, and the `ai/AGENTS.md` rules.
- On base `4e6916d16ac57647105d14a47a6680117d6b5738`, the new semantic source
  regression failed for all twelve scripts. It parses Python ASTs; it does not
  depend on whitespace or comments.

The retained records were therefore not evidence that the implementation was
live. This is the BUG-048 failure mode: a later change can silently undo code
while every narrative artefact still describes the intended state.

## Restored contract

All twelve current scripts now:

- call `bootstrap_ai_script(__file__)` instead of mutating `sys.path`;
- call `make_argument_parser(...)` instead of constructing
  `argparse.ArgumentParser` directly;
- accept `main(argv: list[str] | None = None)` and normalize it once through
  `collect_cli_argv(argv)`;
- parse and record that same normalized vector in run provenance; and
- avoid any direct `sys.argv` read.

The five LOSO scripts importing `ai.scripts.*` or `ai.train.*`, plus
`qat_train.py`, pass `include_repo_root=True`. The helper defaults to adding
`ai/src` only, so omitting that flag makes direct file invocation fail before
argument parsing even though module imports from the repository root work.

## Alternatives considered

No alternatives: only-one-way restoration of accepted ADR-0680/0681 behavior.
Copying the old commits wholesale was rejected because every script has later
functional and CLI changes that must remain intact. Extending the bootstrap's
global default was also rejected because scripts that need only `ai/src` should
not gain a broader import root implicitly.

## Verification boundary

The regression covers exactly the twelve named files and the public CLI setup
seam. Focused tests exercise explicit and `None` argv provenance, QAT/PTQ,
per-execution-provider quantization reports, exporter manifests, and both
direct-file and module `--help` invocation. No LOSO run, training, benchmark,
model export, registry rewrite, or checkpoint mutation is part of this work.

## Verification results

- red-cap on the exact base: 12 failures, one for every named script;
- restored semantic regression: 12 passed;
- focused helper/provenance/QAT/PTQ/exporter set: 74 passed;
- direct-file plus `python -m` `--help`: 24 of 24 invocations passed; and
- complete `ai/` package: 1,327 passed, 1 skipped;
- Black 26.5.1 over all 292 `ai/` files plus Ruff lint and import-order checks:
  clean;
- `make verify-all`: pass, with all 28 scanner-visible touched files clean and
  the repository HISS count reduced to 264 within the 276-item baseline; and
- final writable AGY implementation review: no unresolved behavior finding;
  remediated 7 pre-push mypy findings introduced across `eval_loso_mlp_small.py`,
  `eval_loso_vmaf_tiny_v3.py`, `eval_loso_vmaf_tiny_v4.py`, `export_tiny_models.py`,
  and `test_ai_cli_helper_restoration.py`; `scripts/git-hooks/pre-push-mypy.py`
  passes with 0 introduced findings (601 inherited from merge base).
