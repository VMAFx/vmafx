---
name: bisect-regression
description: Run git bisect against a user-defined failure predicate (numeric diff, perf threshold, test failure). Outputs the first-bad commit with context.
---
<!-- markdownlint-disable MD013 -->

# /bisect-regression

## Invocation

```text
/bisect-regression --bad=<sha> --good=<sha> --predicate=<type> [--predicate-arg=...]
```

Predicates:

- `test:<name>` = repository-sanitized Meson test runner exit code.
- `score-delta:<ref>,<dist>,<feature>,<tol>` = absolute score diff > tol
  between bad commit and good.
- `perf-threshold:<feature>,<pct>` = throughput drop > pct%.
- `netflix-golden` = Netflix CPU golden tests pass/fail.

## Workflow

1. `git bisect start <bad> <good>`.
2. Create script `/tmp/bisect-predicate.sh`:
   - Rebuild (`/build-vmaf --backend=cpu`)
   - Evaluate predicate
   - Exit 0 (good) / 1 (bad) / 125 (skip, e.g. build failed unrelated reason)
3. `git bisect run /tmp/bisect-predicate.sh`.
4. On finish: `git bisect log` + `git show <first-bad>` + markdown summary.
5. `git bisect reset`.

## Guardrails

- Never commit during bisect.
- Stash local changes first; unstash at end.
- Skip commits failing build (exit 125); do not mark bad.

## Shared helpers

- Driver script (`scaffold.sh`) sources
  [`.claude/skills/lib/bisect-common.sh`](../lib/bisect-common.sh) for
  operator-tree guards (clean-tree check, auto-stash push/pop,
  `git bisect reset` on exit) and verdict rendering.
- Companion skill `/bisect-model-quality` sources same library.
- Keep shared helper changes backwards compatible; do not silently
  regress model-quality flow.
