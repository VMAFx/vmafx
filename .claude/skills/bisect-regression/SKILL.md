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

- `test:<name>` — `meson test <name>` exit code.
- `score-delta:<ref>,<dist>,<feature>,<tol>` — absolute score diff > tol between the
  bad commit and good.
- `perf-threshold:<feature>,<pct>` — throughput dropped by more than pct%.
- `netflix-golden` — Netflix CPU golden tests pass/fail.

## Workflow

1. `git bisect start <bad> <good>`.
2. Create a bisect script under `/tmp/bisect-predicate.sh` that:
   - Rebuilds (`/build-vmaf --backend=cpu`)
   - Evaluates the predicate
   - Exits 0 (good) / 1 (bad) / 125 (skip — e.g. build failed for unrelated reasons)
3. `git bisect run /tmp/bisect-predicate.sh`.
4. On finish: `git bisect log` + `git show <first-bad>` + a markdown summary.
5. `git bisect reset`.

## Guardrails

- Never commits during bisect.
- Stashes any local changes first; un-stashes at end.
- Skips commits that fail to build (exit 125) rather than marking them bad.

## Shared helpers

The driver script (`scaffold.sh`) sources
[`.claude/skills/lib/bisect-common.sh`](../lib/bisect-common.sh) for the
operator-tree guards (clean-tree check, auto-stash push/pop, `git bisect reset`
on exit) and verdict rendering. The companion skill `/bisect-model-quality`
sources the same library; keep changes to the shared helpers backwards
compatible so the model-quality flow does not silently regress.
