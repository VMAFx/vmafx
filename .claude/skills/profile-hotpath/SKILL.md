---
name: profile-hotpath
description: Profile hot path (feature + backend) with matching profiler (perf / ncu / Vtune / rocprof), produce flamegraph + top-N hot functions, suggest concrete optimizations. Delegates to perf-profiler agent.
---
<!-- markdownlint-disable MD013 -->

# /profile-hotpath

## Invocation

```text
/profile-hotpath <backend> <feature> [--resolution=576|640|720|1080|4k] [--frames=120]
```

## Behavior

Delegates profiling + interpretation -> `perf-profiler` agent
(see `.claude/agents/perf-profiler.md`):

1. Build (if needed) with `--config=relwithdebinfo` for symbolicated profiles.
2. Select profiler for backend.
3. Run `build/tools/vmaf_bench` with fixed seed + frame count.
4. Produce flamegraph under `build/profiles/<date>/flame.svg`.
5. Emit top-10 hot-function list + recommendations.

## Notes

- `perf-profiler` agent owns interpretation — skill only sets up environment
  and invokes it.
- Profiler not installed (`ncu` missing for CUDA, `vtune` for SYCL) -> degrade
  to `perf record` fallback with clear note in report.
