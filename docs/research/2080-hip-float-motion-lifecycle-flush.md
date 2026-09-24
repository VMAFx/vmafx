# Research-2080: HIP float-motion lifecycle and flush regression — 2026-09-24

## Finding

Canonical BUG048 section A5 names two protections that were merged by
[`4c7315fc6`](https://github.com/VMAFx/vmafx/commit/4c7315fc6a8f29e6cdc7701e4851ceda813fcbd4)
and then removed wholesale by the unrelated mega-merge
[`c2a3c7e0f`](https://github.com/VMAFx/vmafx/commit/c2a3c7e0febd98a6831b9bfebe5e298e229b1244):

1. `motion_force_zero=true` replaced the cloned extractor's `close` callback
   with `NULL` after allocating `feature_name_dict`. The surrounding init path
   releases the HIP context and lifecycle immediately, but no later callback
   owns the dictionary, so every successfully closed force-zero context leaks
   it.
2. `flush_fex_hip()` always appended the tail `motion2` sample. Calling the
   flush callback again for the same collector/index attempted to overwrite an
   existing score and returned an error instead of reporting that the extractor
   was already drained.

This is a restoration, not a new architecture decision. No ADR is needed: the
lifecycle and idempotency contracts were already selected and shipped; this
change restores them with a regression test that covers the current code shape.

## Current-tree lifecycle

`init_fex_hip()` creates a HIP context and kernel lifecycle before entering the
force-zero branch. That branch builds the option-derived feature-name
dictionary, after which `init_fex_hip()` deliberately calls
`fm_hip_release_device()` before returning. The remaining owned object is the
dictionary. Reusing `close_fex_hip()` is safe and narrower than adding a second
teardown implementation: `fm_hip_release()` tolerates the already-null device
handles and then frees the dictionary.

The original flush guard queried the literal
`VMAF_feature_motion2_score`. That is insufficient in the current tree because
`motion_fps_weight` is a feature parameter and
`vmaf_feature_collector_append_with_dict()` publishes an option-derived name.
The regression uses `motion_fps_weight=1.5` deliberately. The guard must resolve
the same name through `feature_name_dict` before probing the collector or it
will miss the existing score and re-append it.

## Decision matrix

| Option | Resource safety | Flush coverage | Decision |
| --- | --- | --- | --- |
| Keep `close=NULL` and unconditional append | Leaks the force-zero dictionary | Repeated flush fails | Rejected: reproduced A5 defects |
| Restore a dictionary-only close helper and probe the literal base name | Frees today's remaining object | Misses option-derived feature names | Rejected: duplicates teardown and leaves a reachable repeated-flush failure |
| Move force-zero before HIP context/lifecycle init | Avoids creating device objects | Does not address flush | Rejected: broader init-order and device-validation behavior change |
| Reuse null-safe `close_fex_hip()` and probe the dictionary-resolved name | One teardown owner | Covers default and parameterized names | Chosen: narrow restoration with one lifecycle owner |

## Executable evidence

The existing `test_hip_float_motion_parity` target now clones the registered
extractor through the real context API. Two red caps were observed on the
pre-fix implementation on an AMD gfx1036:

- force-zero init failed `HIP force-zero init must retain a
  dictionary-owning close callback`;
- two raw tail flushes failed `HIP float-motion repeated tail flush must be
  idempotent`.

The test restores the registered close callback before cleaning up the red
force-zero case, so the regression itself does not repeat the leak. All
resources are released before assertions. The flush case submits and collects
two real frames, resolves the option-derived score keys, calls the raw flush
callback twice, and verifies both the expected `last motion * 1.5` tail and the
value retained after the repeated flush.

Reproducer inside the current development image:

```sh
docker run --rm --user 1000:1000 \
  --device=/dev/kfd:/dev/kfd --device=/dev/dri:/dev/dri \
  --security-opt seccomp=unconfined --group-add 984 --group-add 988 \
  --entrypoint bash -v "$PWD:/workspace" -w /workspace vmaf-dev-mcp:local \
  -lc 'ninja -C build-a5-hip test/test_hip_float_motion_parity && \
       meson test -C build-a5-hip test_hip_float_motion_parity --print-errorlogs'
```

Post-fix result on the same gfx1036: `1/1 OK` in 0.20 seconds. The change does
not modify a public header, FFmpeg patch, score formula, snapshot, or Netflix
golden assertion.

Broader post-fix verification on the same tree:

- HIPCC-enabled serial `fast` suite: 197 passed, including every small and
  large HIP parity target;
- `enable_hip=true`, `enable_hipcc=false` scaffold target: 1 passed, with the
  direct submit case taking its documented `-ENOSYS` skip;
- Netflix CPU golden gate: 271 passed, 12 skipped;
- `praetorctl audit`: 276 active HISS findings within the 276 baseline, with
  every touched supported file clean;
- scoped exhaustive Cppcheck: no actionable findings; scoped HIP clang-tidy:
  zero file-attributed findings in both touched C translation units (the six
  reported findings are pre-existing include-header debt). The test TU keeps
  its portable C `NULL` spellings inside the repository-standard ADR-1138 file
  bracket; its function-size and branch-count debt was removed by
  refactoring rather than suppressed.
