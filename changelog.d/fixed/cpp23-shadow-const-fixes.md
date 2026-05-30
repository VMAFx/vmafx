- Replaced all C-style casts (`(char*)`, `(void*)`, `(uint8_t*)`, `(size_t)`,
  `(unsigned)`, `(decltype(...))`) with `static_cast<>` in
  `core/src/feature/feature_collector.cpp` and `core/src/sycl/common.cpp`.
- Renamed local variable `capacity` → `new_capacity` in
  `core/src/fex_ctx_vector.cpp` to eliminate the `-Wshadow` hit against the
  `rfe->capacity` struct member.
- No behaviour change; the fixes are type-system-only (ADR-0839).
