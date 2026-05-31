<!-- markdownlint-disable MD060 -->
# Research: Logging consistency audit in fork-added C/C++ code

- **Status**: Active
- **Workstream**: PR fix/logging-consistency-audit (this PR)
- **Last updated**: 2026-05-30

## Question

libvmaf ships a user-installable log callback `vmaf_log()` filtered by
`vmaf_set_log_level()`. Fork-added C/C++ code in `core/src/` has
accumulated direct `printf` / `fprintf(stderr, ...)` calls that bypass
this surface entirely. Which call sites are real diagnostics that should
go through the callback, and which are intentional CLI stdout output
that must stay as direct prints?

## Sources

- `core/src/log.h` — declares `vmaf_log(VmafLogLevel, fmt, ...)` and the
  level setter.
- `core/include/libvmaf/libvmaf.h` — public surface for
  `vmaf_set_log_callback` (the user contract that direct stderr writes
  break).
- Memory note `feedback_no_lint_skip_upstream` — lint upstream-mirror
  with the same rules as fork-added.
- Memory note `feedback_vendored_in_scope` — vendored libsvm /
  cJSON / pdjson are nominally in scope, but fixing must not break
  upstream sync semantics.

## Findings

Enumerated all `printf` / `fprintf(stderr, ...)` calls in
`core/src/**/*.{c,cpp}` (excluding `third_party/`, `iqa/`, `3rdparty/`).
Classified each site against three axes:

1. **Is it a diagnostic, or intended CLI output?** Diagnostics belong in
   `vmaf_log`. CLI output (e.g., `--list-devices`, `--print-timing`,
   profiling tables) is the contract of the function and must stay
   direct.
2. **Is it the log implementation itself?** `core/src/log.{c,cpp}`
   contain the default callback; they must use raw `fprintf` by
   construction.
3. **Is it inside vendored code with upstream-sync invariants?**
   Vendored libsvm in `core/src/svm.cpp` carries its own `fprintf`
   pattern — see deferral below.

| Site | Decision | Reason |
|---|---|---|
| `core/src/libvmaf.c` x5 — `vmaf_write_output` guards | ROUTE through `vmaf_log` (ERROR) | Public API error path; users expect their callback to receive the message. |
| `core/src/sycl/dispatch_strategy.cpp` — `VMAF_SYCL_NO_GRAPH` deprecation | ROUTE (WARNING) | One-shot env-var deprecation, exactly the canonical WARNING use case. |
| `core/src/sycl/common.cpp` — device-enum exception | ROUTE (ERROR) | Exception path during `vmaf_sycl_list_devices()`. |
| `core/src/sycl/common.cpp` — upload-timing debug print | ROUTE (DEBUG) | Already gated on `extractor_timing` debug flag; DEBUG level fits. |
| `core/src/sycl/common.cpp` — graph_submit exceptions x3 | ROUTE (ERROR) | All exception paths. |
| `core/src/sycl/common.cpp` — `vmaf_sycl_list_devices` stdout | KEEP | `--list-devices` is a CLI surface; stdout is the contract. |
| `core/src/sycl/common.cpp` — `vmaf_sycl_print_timing` | KEEP | Explicit user-invoked print; stderr stream is the contract. |
| `core/src/sycl/common.cpp` — `vmaf_sycl_profiling_print` | KEEP | Explicit user-invoked profiling table. |
| `core/src/log.{c,cpp}` | KEEP | The log implementation itself. |
| `core/src/svm.cpp` (libsvm warnings/errors) | DEFER | Vendored libsvm; rerouting through `vmaf_log` would change the semantics for callers that embed libsvm directly. Worth its own audit + ADR if pursued. |
| `core/src/feature/{vif,adm,ssim,motion,…}.c` `printf("error: ...")` | DEFER | Upstream Netflix feature-extractor code; same upstream-sync concern as libsvm. Track as follow-up. |
| `core/tools/vmaf.c` and other CLI tools | KEEP | stdout score output IS the CLI interface. |

## Decisions

- Route the **11 fork-local diagnostic sites** through `vmaf_log` at the
  appropriate level (5 ERROR in `libvmaf.c`, 1 WARNING + 4 ERROR + 1
  DEBUG across SYCL).
- Keep CLI-style stdout/stderr prints whose stream IS the intended
  interface.
- Defer vendored libsvm and upstream-mirror feature-extractor sites to a
  follow-up — they need an upstream-sync impact analysis first.

## Alternatives considered

1. **Reroute everything including vendored libsvm.** Rejected for now —
   changes libsvm-internal warning semantics; bigger surface, deserves
   its own ADR.
2. **Route the CLI device-list / profiling prints through
   `vmaf_log(INFO)`.** Rejected — these are pull-style "print on
   request" functions whose stream IS the contract. Routing through the
   callback would silently drop output for callers that haven't
   installed a callback at INFO level.
3. **Add a separate `vmaf_log_diagnostic` helper.** Rejected — the
   existing `vmaf_log(level, ...)` already covers the routing need; no
   case for a parallel surface.

## Validation

- `meson setup build-log core -Denable_cuda=false -Denable_sycl=false &&
  ninja -C build-log` succeeds.
- `meson test -C build-log --suite=fast` passes 49/49.
- SYCL backend builds gated on CI (no host SYCL toolchain here); the
  `extern "C" { #include "log.h" }` wrapping matches the pattern already
  used in `core/src/sycl/common.cpp`.

## Follow-ups

- Vendored `core/src/svm.cpp` audit — separate PR, requires upstream
  libsvm-sync impact note.
- Upstream-mirror feature extractors (`vif.c`, `adm.c`, `ms_ssim.c`,
  `motion.c`, `ssim.c`) — separate PR, port back to Netflix or treat as
  permanent fork delta.
