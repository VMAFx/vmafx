---
paths:
  - core/src/metadata_handler.cpp
  - core/src/metadata_handler.h
invariant: C++20 metadata handler preserves extern "C" symbols and unique_ptr lifecycle.
area: engine
---
<!-- markdownlint-disable MD013 -->
# Metadata handler C++20 pilot and C ABI symbols

## 9. `metadata_handler.cpp` — C++20 pilot; keep `extern "C"` (ADR-0708)

`core/src/metadata_handler.cpp` (previously `metadata_handler.c`) = first
C++20 internal implementation TU. `metadata_handler.h` carries `extern "C"`
guards that preserve C linkage for C and C++ callers, including
`feature_collector.cpp`, without link-name-mangling mismatch.

Never:

- Remove `extern "C"` guards from `metadata_handler.h`.
- Rename three public symbols (`vmaf_metadata_init`, `vmaf_metadata_append`,
  `vmaf_metadata_destroy`).
- Move file back to `.c` — `unique_ptr` and `CallbackListDeleter` require
  C++ compiler.

When porting upstream Netflix/vmaf commit modifying original
`core/src/metadata_handler.cpp`: apply diff content to
`core/src/metadata_handler.cpp` (C code valid C++; `extern "C"` block
in header stays). Run `make test-netflix-golden` post-port.
