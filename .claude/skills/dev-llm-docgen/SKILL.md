---
name: dev-llm-docgen
description: Draft Doxygen @brief/@param/@return block for named function in C/C++/CUDA file via local LLM (Ollama).
---
# /dev-llm-docgen

## Invocation

```text
/dev-llm-docgen <file> <symbol> [--model <name>]
```

## Steps

1. Verify `vmaf-dev-llm check` = green.
2. Verify symbol exists in file (grep `<symbol>\\s*\\(`). If not -> abort with
   clear error.
3. Run `vmaf-dev-llm docgen --file <file> --symbol <symbol>` -> capture stdout
   as draft docblock.
4. Show draft to user; ask choice:
   - **insert** -> put above symbol declaration (use `Edit` tool)
   - **copy** -> print verbatim for manual paste
   - **regenerate** -> rerun step 3

   Do NOT insert automatically.

## Guardrails

- Never rewrite existing docblock unless user says `replace`.
- Reject files under `subprojects/` (vendored upstream).
- Generated block must start with `/**` and end with `*/`; otherwise treat as
  bad response -> regenerate.
- For files matching `*.cu` / `*.cuh`, use `review_cuda.md` context -> prefer
  CUDA-appropriate parameter descriptions (e.g., "pinned host pointer",
  "device-resident array").
