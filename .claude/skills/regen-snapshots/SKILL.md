---
name: regen-snapshots
description: Regenerate fork-added test snapshot JSONs under testdata/ (scores_cpu_*.json, netflix_benchmark_results.json) after an intentional numerical change. Requires justification committed to the message.
---
<!-- markdownlint-disable MD013 -->

# /regen-snapshots

## Invocation

```text
/regen-snapshots --justification="<short rationale>" [--files=scores_cpu_640,scores_cpu_576,...]
                 [--backend=cpu|cuda|sycl|all]
```

## Steps

1. Refuse if `--justification` missing or empty.
2. Build requested backend (`/build-vmaf --backend=<backend>`).
3. For each snapshot in `--files` (default: all `testdata/scores_cpu_*.json` and
   `testdata/netflix_benchmark_results.json`):
   - Locate matching regeneration script (`testdata/gen_cpu_golden.py`,
     `testdata/run_sycl_scores.py`, `testdata/benchmark_netflix.py`).
   - Run script, write to tempfile.
   - Diff old vs new; identical -> skip.
   - Otherwise replace.
4. Emit diff summary: file, frames affected, max delta observed.
5. Prepare commit message draft:

   ```text
   test(snapshots): regenerate <files>

   Justification: <justification>

   Affected: <summary>
   ```

   Print draft; do NOT auto-commit.

## Guardrails

- Never touch `python/test/**` — holds Netflix golden assertions (see §8 of
  CLAUDE.md). Regeneration script tries to write there -> ABORT.
- Max delta > 0.5 (absolute) without justification mentioning model/feature
  rewrite -> abort with error.
