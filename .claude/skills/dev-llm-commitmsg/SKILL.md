---
name: dev-llm-commitmsg
description: Draft a Conventional-Commits message from the current `git diff --staged` using a local LLM (Ollama). User edits before commit.
---
<!-- markdownlint-disable MD013 -->

# /dev-llm-commitmsg

## Invocation

```text
/dev-llm-commitmsg [--model <name>]
```

## Steps

1. Verify `git diff --staged` has content. If empty -> tell user stage
   changes first (`git add -p`), exit.
2. Verify `vmaf-dev-llm check` = green (see `dev-llm-review` pre-flight).
3. Run `vmaf-dev-llm commitmsg` (add `-m <model>` if provided).
   Capture stdout = draft.
4. Present draft, ask user:
   - **accept** -> emit `git commit -m "<heredoc>"` using draft verbatim,
   - **edit** -> open `$EDITOR` with draft, let user refine,
   - **regenerate** -> rerun step 3.

   Do NOT commit automatically — user must confirm.

## Guardrails

- Refuse run on `master` or protected branch.
- Strip `Co-Authored-By:` or `Signed-off-by:` lines from draft — added by
  pipeline.
- First line must match
  `^(feat|fix|perf|refactor|docs|test|build|ci|chore|revert)(\\([a-z0-9-]+\\))?: .+`
  before acceptance; otherwise regenerate.
