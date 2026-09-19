---
name: dev-llm-modelcard
description: Draft a Markdown model-card for a shipped ONNX tiny-AI model by collecting hard facts (graph, sidecar, op allowlist, optional live PLCC/SROCC) and handing them to the local LLM.
---
# /dev-llm-modelcard

## Invocation

```text
/dev-llm-modelcard <onnx-path> [--features <parquet>] [--split test|val|train|all] [--model <ollama-model>]
```

## Steps

1. Verify `vmaf-dev-llm check` = green. If not, abort -> point user
   at `ollama serve`.
2. Resolve ONNX path. Reject path outside repo unless user explicitly
   confirms (! random host file almost never intended).
3. Locate model sidecar or exporter manifest. Prefer sidecars with
   `run_provenance.schema == "ai-run-provenance-v1"`. If none exists, report
   artifact missing replay evidence before drafting card.
4. Run `vmaf-dev-llm modelcard --onnx <path> [--features <parquet>]
   [--split …] [--facts-only]` x2:
   - First with `--facts-only`: show user fact block sent to LLM = trust
     boundary; LLM cannot invent past this point.
   - If user approves, rerun without `--facts-only` -> get rendered card.
5. Show draft. Offer 3 actions:
   - **save** — write to `model/<name>.md` (beside `.onnx` / `.json`) using
     `Write` tool.
   - **copy** — print verbatim for user paste.
   - **regenerate** — rerun step 3, optionally with `--model` overridden.
6. Never save automatically; model cards = published artifacts.

## Guardrails

- Facts block = source of truth. If field missing, rendered card must say
  "not recorded", not invent value. Flag output contradicting facts block.
- Missing `run_provenance` = real documentation gap. Do not hide in prose;
  either fix producing script via `/ai-run-manifest` or mark card provenance
  as unavailable.
- Always pass `--repo-root` pointing at repo (defaults to CWD; invoke skill
  from repo root).
- If `--features` = dataset user did not train on, surface in card under
  "Measured quality" — PLCC on out-of-distribution split = useful data point,
  should not be conflated with training performance.
- Do NOT run this skill on models under `subprojects/` — vendored upstream,
  ship own documentation.

## Typical uses

- Publish model: before merge new tiny-AI model into `model/tiny/`, generate
  card, commit `.md` alongside.
- Audit shipped model: reader seeing .onnx in `model/` can run skill to get
  structured summary of provenance + contract.
