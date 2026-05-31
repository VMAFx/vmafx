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

1. Verify `vmaf-dev-llm check` is green. If not, abort and point the user
   at `ollama serve`.
2. Resolve the ONNX path; reject anything outside the repo unless the
   user explicitly confirms (model cards for random host files are
   almost never what the user wants).
3. Locate the model sidecar or exporter manifest. Prefer sidecars that
   contain `run_provenance.schema == "ai-run-provenance-v1"`; if none
   exists, report that the artifact is missing replay evidence before
   drafting the card.
4. Run `vmaf-dev-llm modelcard --onnx <path> [--features <parquet>]
   [--split …] [--facts-only]` twice:
   - First with `--facts-only` to show the user the fact block that will
     be handed to the LLM — this is the trust boundary; the LLM cannot
     invent anything past this point.
   - If the user approves, run it again without `--facts-only` to get
     the rendered card.
5. Show the draft. Offer three actions:
   - **save** — write it to `model/<name>.md` (beside the `.onnx` /
     `.json`), using the `Write` tool.
   - **copy** — print verbatim for the user to paste.
   - **regenerate** — re-run step 3, optionally with `--model` overridden.
6. Never save automatically; model cards are published artifacts.

## Guardrails

- The facts block is the source of truth. If a field is missing, the
  rendered card must say "not recorded", not invent a value. Flag any
  output that contradicts the facts block.
- Missing `run_provenance` is a real documentation gap. Do not hide it
  in prose; either fix the producing script via `/ai-run-manifest` or
  mark the card's provenance as unavailable.
- Always pass `--repo-root` pointing at the repo (defaults to CWD, so
  invoke the skill from the repo root).
- If `--features` is a dataset the user did not train on, surface that
  in the card under "Measured quality" — PLCC on an out-of-distribution
  split is a useful data point but should not be conflated with
  training performance.
- Do NOT run this skill on models under `subprojects/` — those are
  vendored upstream and ship their own documentation.

## Typical uses

- Publishing a model: before merging a new tiny-AI model into
  `model/tiny/`, generate its card and commit the `.md` alongside.
- Auditing a shipped model: a reader who sees a .onnx in `model/` can
  run this skill to get a structured summary of provenance + contract.
