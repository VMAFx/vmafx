# FFmpeg release patch lifecycle evidence

The previous checker could succeed after fetch failure and reused a checkout
with reset/clean. Patch 0018 duplicated percentile cases from patch 0005 and
failed cumulative replay against n9.0.1. The required CI aggregator did not
cover full-series replay. These are source and execution findings from
2026-09-08, not inferred release readiness.

The repaired series applies all 18 patches to the official released tag
n9.0.1, commit `bf1b838f2ab88b4f8fd83443325c782ea0e0f7fa`. The extracted
pool mapper compiles and passes checks with and without percentile support.
The maintained patch automation has real Git fixtures for stable tag selection,
rebase, idempotence, network and conflict failures, inherited hook variables,
hostile global Git configuration and rollback after a replacement failure.
Workflow contracts check impact routing, required registration, failure
propagation through diagnostic tee and scheduled-only upstream discovery.

Reproduce the fixture suite with:

```bash
python3 -m unittest discover -s scripts/ci -p 'test_ffmpeg_patch*.py' -v
python3 scripts/ci/ffmpeg_patch_stack.py --check
```

This evidence validates patch maintenance; it does not replace the FFmpeg
integration builds, numerical golden gate or complete release acceptance.
See [ADR-1240](../adr/1240-ffmpeg-release-patch-lifecycle.md) for alternatives.
