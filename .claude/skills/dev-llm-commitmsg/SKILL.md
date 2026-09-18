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

1. Verify `git diff --staged` has content. Empty -> tell user stage changes
   first (`git add -p`), exit. No staged diff = nothing to summarize, model
   has no input to read.
2. Verify `vmaf-dev-llm check` = green (see `dev-llm-review` pre-flight).
   Red backend -> draft quality unreliable, skip generation, surface backend
   error instead.
3. Run `vmaf-dev-llm commitmsg` (add `-m <model>` if provided). Capture
   stdout = draft. Model choice changes wording only, never changes which
   diff gets read.
4. Present draft, ask user:
   - **accept** -> emit `git commit -m "<heredoc>"` using draft verbatim,
   - **edit** -> open `$EDITOR` with draft, let user refine, save closes
     editor and continues to commit,
   - **regenerate** -> rerun step 3, discard prior draft entirely.

   Do NOT commit automatically — user confirms every single run, no
   exceptions.

## Guardrails

- Refuse run on `master` or protected branch. Commit-msg drafting precedes
  real commit -> block at source, not after.
- Strip `Co-Authored-By:` or `Signed-off-by:` lines from draft — added later
  by pipeline; duplicate lines break trailer parsing downstream.
- First line must match
  `^(feat|fix|perf|refactor|docs|test|build|ci|chore|revert)(\\([a-z0-9-]+\\))?: .+`
  before acceptance; otherwise regenerate. Non-conforming subject line fails
  commit-msg git hook later, wastes whole draft cycle.
