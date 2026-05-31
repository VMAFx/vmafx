<!-- markdownlint-disable MD060 -->
# Research-0675: Per-EP quantisation report provenance

## Question

`ai/scripts/measure_quant_drop_per_ep.py` writes the JSON and Markdown evidence
used to refresh CPU/CUDA/OpenVINO PTQ findings, but the JSON report did not
record the exact registry, fp32 baselines, EP list, hardware tag, argv, or
output targets that produced the table. That made copied model-card evidence
harder to replay after dependency or hardware changes.

## Finding

The report is a durable investigation artifact even though it is normally
written under gitignored `runs/`. It feeds the PTQ research digest and any
future model-card promotion that relies on non-CPU execution providers. The
right contract is the shared ADR-0661 `run_provenance` block rather than a
script-local manifest shape.

## Decision

Add `run_provenance` to `results.json` with:

- entrypoint: `ai/scripts/measure_quant_drop_per_ep.py`
- inputs: `model/tiny/registry.json` and the optional `--extra-fp32` baselines
- args: selected EPs, OpenVINO device, hardware tag, budgets, gate flag, output
  directory
- outputs: `results.json` and `results.md`

The Markdown table remains human-facing and unchanged; the machine-readable
JSON gets the replay metadata.

## Alternatives Considered

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| Keep the existing JSON shape | No downstream schema delta | PTQ evidence still loses hardware/argv/model-input lineage | Rejected |
| Add a quant-specific metadata block | Could be smaller | Duplicates ADR-0661 path hashing and argument normalization | Rejected |
| Put provenance only in Markdown | Easy for humans | Bad for scripts and model-card automation | Rejected |

## Validation

- `.venv/bin/ruff check ai/scripts/measure_quant_drop_per_ep.py ai/tests/test_measure_quant_drop_per_ep.py`
- `.venv/bin/python -m pytest ai/tests/test_measure_quant_drop_per_ep.py -q`
- `.venv/bin/mkdocs build --strict`
- `make format-check`
