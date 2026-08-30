# Research 0700: AI CLI Helper Pattern

## Question

Which remaining AI manifest boilerplate is duplicated enough to extract now
without turning each materializer into a generic framework?

## Inputs Reviewed

- `ai/scripts/batch_materialize_saliency_features.py`
- `ai/scripts/batch_materialize_second_opinion_features.py`
- `ai/scripts/batch_materialize_mos_labels.py`
- `ai/src/aiutils/run_manifest.py`
- `ai/src/aiutils/AGENTS.md`
- `.claude/skills/ai-run-manifest/SKILL.md`

## Findings

The three batch materializer scripts intentionally differ in table schema,
per-row materializer calls, and summary fields. Those pieces should remain
local because they encode distinct domain rules: saliency row failures,
second-opinion scorer joins, and MOS label match-rate enforcement.

They do not differ in wrapper behavior. Each exposes `--manifest`,
`--base-dir`, optional JSON and Markdown reports, a fail-fast mode, and raw
argv capture for ADR-0661 run provenance. The saliency runner adds one
compatible row-failure override flag. Extracting only those shared flags removes
the repeated parser boilerplate while avoiding a generic batch framework.

## Decision Input

Create `aiutils.cli_helpers` with three narrow helpers:

- `make_argument_parser()` for standard parser construction.
- `collect_cli_argv()` for canonical raw-argv capture.
- `add_batch_manifest_arguments()` for shared batch-runner flags.

Update the three existing batch materializers, `ai/AGENTS.md`,
`ai/src/aiutils/AGENTS.md`, and the `/ai-run-manifest` skill so future scripts
reuse the pattern instead of copying the old blocks.

## Reproducer / Smoke

```bash
.venv/bin/python -m pytest \
  ai/tests/test_cli_helpers.py \
  ai/tests/test_batch_materialize_saliency_features.py \
  ai/tests/test_batch_materialize_second_opinion_features.py \
  ai/tests/test_batch_materialize_mos_labels.py -q
```

## Limits

This does not consolidate manifest loading, table validation, Markdown report
content, or materializer execution. Those remain intentionally local until at
least one more package proves the same abstraction is worth carrying.
