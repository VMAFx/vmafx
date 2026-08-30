---
name: add-feature-extractor
description: Scaffold a new feature extractor (e.g. a novel metric) with C source+header, registry entry, doc stub, and a smoke test. Does not produce a SIMD or GPU path — those come via /add-simd-path and /add-gpu-backend.
---

<!-- markdownlint-disable MD060 -->

# /add-feature-extractor

## Invocation

```text
/add-feature-extractor <name> [--type=full-reference|no-reference]
```

## Files created

| Path                                        | Purpose                                    |
|---------------------------------------------|--------------------------------------------|
| `core/src/feature/<name>.c`              | Scalar reference implementation            |
| `core/src/feature/<name>.h`              | Prototype                                  |
| `core/test/test_<name>.c`                | Smoke test (1 frame, fixed expected value) |
| `docs/<name>.md`                            | Metric documentation (inputs, range, refs) |

## Files patched

- `core/src/feature/feature_extractor.c` — registry row.
- `core/src/feature/all.c` — `#include "<name>.h"` + array entry.
- `core/src/meson.build` — new source in the feature set.
- `core/test/meson.build` — register test.

## Template fills

- Extractor struct: `extract` pointer, name, type (FR/NR), features exposed.
- No SIMD / GPU variants wired — the feature first exists as a scalar implementation
  and is the reference against which SIMD/GPU paths are later tested.

## Guardrails

- Names collide-checked against the existing registry.
- The smoke test must actually pass (non-NaN, finite) against the sample YUVs in
  `python/test/resource/yuv/`.
