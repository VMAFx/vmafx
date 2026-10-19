---
paths:
  - core/src/log.c
  - core/src/log.h
  - core/src/log.cpp
invariant: Fork diagnostics route through vmaf_log with BUG-048 format lock; log.c respects C23 va_start.
---
<!-- markdownlint-disable MD013 -->
# Logging callbacks, fork diagnostic routing, and C23 va_start

## 10. Fork diagnostics route through `vmaf_log`, not `fprintf(stderr)`

libvmaf exposes user-installable log callback via
`vmaf_set_log_callback` and level filter via `vmaf_set_log_level`.
Direct `fprintf(stderr, ...)` / `printf(...)` bypasses both surfaces —
message reaches terminal regardless of user's installed
callback or chosen verbosity, and embedded callers (FFmpeg filter, MCP
server, future bindings) can never capture it.

For any new fork-local diagnostic (errors, warnings, debug traces),
use `vmaf_log(VMAF_LOG_LEVEL_{ERROR,WARNING,INFO,DEBUG}, fmt, ...)`
declared in [`log.h`](../log.h). C++ TUs include header inside
`extern "C" { }` block — see `core/src/sycl/common.cpp` and
`core/src/sycl/dispatch_strategy.cpp` for pattern.

**BUG-048 format regression lock:** diagnostics guarded by
`core/test/test_vmaf_log_callsite_format.py` are one complete record per call.
Keep their trailing `\n`; keep CUDA initialization message bodies free of
`Error:` because `VMAF_LOG_LEVEL_ERROR` already renders that severity. This
includes original `9d57a93bf` sites and later second CUDA-init failure
path.

Exceptions — direct stream writes are correct in these cases:

- `core/src/log.{c,cpp}` — log implementation itself.
- CLI tools under `core/tools/` — stdout score / JSON output is
  contract.
- Pull-style "print on request" SYCL APIs
  (`vmaf_sycl_list_devices`, `vmaf_sycl_print_timing`,
  `vmaf_sycl_profiling_print`) — stream IS function's contract;
  routing through callback would silently drop output for callers
  without installed callback at matching level.
- Vendored libsvm (`core/src/svm.cpp`) and upstream-mirror feature
  extractors (`feature/vif.c`, `feature/adm.c`, etc.) — leave as-is to
  preserve upstream-sync semantics; route only if touching PR has
  upstream-sync impact note.

`log.c` has one additional C23 toolchain invariant: Clang lowers `va_start`
to `__builtin_c23_va_start`, which Clang 21's VA-list analyzer does not model.
Keep Clang+C23 branch on semantically identical
`__builtin_va_start(args, fmt)` while GCC and MSVC retain standard macro.
Do not replace internal `log.h` guard with identifier beginning `__`;
that namespace is reserved by ISO C and fails CERT DCL37-C.

See `docs/research/logging-consistency-audit-2026-05-30.md` for
audit that established this invariant.

## Process log level atomic (T-LOG-LEVEL-GLOBAL-DATA-RACE-2026-10-06)

`log.cpp`: `vmaf_log_level` and `istty` are `std::atomic<int>`, relaxed store in `vmaf_set_log_level()`, relaxed load in `vmaf_log()` (`istty` read once per line into `tty`). `vmaf_init()` sets level on any thread; `vmaf_log()` reads on every thread, workers included. Plain globals = data race (TSan, `core/test/test_log_level_threads.c`, nightly / master TSan jobs). Upstream log sync keeps the atomics. `log.c` not built (ADR-0708), unchanged.
