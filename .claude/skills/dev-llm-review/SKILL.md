---
name: dev-llm-review
description: Run a local LLM code review via vmaf-dev-llm (Ollama-backed) against a specific file. Dev-time self-review pass before PR.
---
# /dev-llm-review

## Invocation

```text
/dev-llm-review <path> [--model <name>]
```

## Steps

1. Verify tool installed: `vmaf-dev-llm --help` must exit 0. Not installed
   -> instruct user to `pip install -e dev-llm` -> exit, no fallback.
2. Verify Ollama reachable: `vmaf-dev-llm check` must exit 0. Unreachable ->
   instruct user to `ollama serve` and pull default model:
   `ollama pull qwen2.5-coder:7b`.
3. Run `vmaf-dev-llm review --file <path>` (add `-m <model>` if provided).
4. Print review output verbatim. Do not rewrite findings: user sees exactly
   what local model produced, no editorializing added on top.

## Guardrails

- Never writes to file being reviewed. Read-only, no side effects.
- Does not send file contents to any cloud service: backend defaults to
  local Ollama, stays on-machine.
- If `path` inside `subprojects/` (vendored upstream), refuse -> suggest
  reviewing fork overlay instead of vendored copy.

## Output format

```text
## Review of <path> (model: <name>)

<model output verbatim>
```
