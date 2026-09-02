# Research digest — ADR-1061: vendored pdjson/cJSON depth-limit and overflow fixes

**Status**: no external research needed — all findings are direct
code-inspection results.

## Summary

Five bugs were identified by static inspection of two vendored source
files (`core/src/pdjson.c`, `core/src/mcp/3rdparty/cJSON/cJSON.c`).
No external sources or literature were required because the bugs are
local to code that was committed without the requisite guards.

| # | File | Kind | Location | Finding |
| - | --------- | -------- | ------------ | ------- |
| 1 | pdjson.c | Missing guard | `push()` L56 | `PDJSON_STACK_MAX` never defined; depth guard compiled out |
| 2 | pdjson.c | INT30-C | `push()` L65 | `(stack_size + INC) * sizeof(...)` — no addition or multiply overflow guard |
| 3 | pdjson.c | INT30-C | `pushchar()` L157 | `string_size * 2` — no overflow guard when `string_size >= SIZE_MAX/2` |
| 4 | cJSON.c | Banned API | 12 sites | ADR-0683 never implemented; `sprintf`/`strcpy` remain |
| 5 | cJSON.c | INT31-C | `cJSON_GetArraySize` | `(int)size` wraps for arrays > `INT_MAX`; upstream FIXME acknowledged |

## References

- CERT-C INT30-C: unsigned integer wraparound.
- CERT-C INT31-C: conversion between integer types.
- ADR-0683 (accepted 2026-05-22) — prior decision mandating cJSON
  banned-function fixes.
- pdjson upstream: <https://github.com/skeeto/pdjson> (Unlicense).
- cJSON upstream: <https://github.com/DaveGamble/cJSON> v1.7.18 (MIT).
