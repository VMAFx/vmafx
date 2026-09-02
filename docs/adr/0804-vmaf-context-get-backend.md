# ADR-0804: Add `vmaf_context_get_backend` — additive ABI introspection

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: Lusoris
- **Tags**: api, abi, backend, gpu, fork-local

## Context

The Phase 4b.8 ABI scoping work (PR #109, ADR-0767) catalogued nine proposed
ABI-breaking changes. One of the nine was `vmaf_context_get_backend`, but on
closer inspection it is purely additive: a new entry point plus a new enum
(`VmafBackend`) appended to `libvmaf.h`. It can ship immediately without
waiting for the full ABI break window.

Callers currently have no way to introspect which compute backend is active in
a `VmafContext`. This matters in three concrete scenarios:

1. **Logging / diagnostics** — a wrapper library or MCP tool wants to report
   which backend is in use without re-reading the init parameters.
2. **Conditional feature use** — a caller may want to skip GPU-specific
   operations (e.g. preallocating pinned memory) if the context turns out to
   be CPU-only.
3. **Test assertions** — parity tests benefit from verifying that the backend
   they configured actually took effect.

## Decision

Add `enum VmafBackend` and `int vmaf_context_get_backend(VmafContext *vmaf,
enum VmafBackend *out)` to `core/include/libvmaf/libvmaf.h`. The implementation
tracks the active backend via a new `active_backend` field in the internal
`VmafContext` struct, set by each `vmaf_<backend>_import_state()` call. A
CPU-only context (no import) returns `VMAF_BACKEND_UNKNOWN` (0).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Defer until full ABI break (ADR-0767) | Keeps all ABI changes in one window | Blocks useful introspection for months | Change is additive; no reason to defer |
| Expose backend as a `VmafConfiguration` read-back field | No new function needed | Conflates init config with runtime state; config field may differ from the backend actually activated | Separate function is cleaner |
| Return backend as a `const char *` string | Human-readable without lookup | Non-exhaustive, breaks `switch` pattern, harder to compare | Enum is idiomatic C |

## Consequences

- **Positive**: Callers can introspect the active backend at runtime without
  re-reading their own init parameters. Test assertions can verify backend
  selection. Fully additive — one new enum and one new function.
- **Negative**: `VmafContext` grows by one `enum VmafBackend` field (4 bytes,
  negligible). The internal `struct VmafContext` is not ABI-stable (callers
  only hold the opaque pointer), so the field addition is safe.
- **Neutral / follow-ups**: `VMAF_BACKEND_VULKAN = 5` is reserved in the enum
  even though the Vulkan backend was removed in ADR-0726, to avoid future
  numbering gaps. The `vmaf_vulkan_import_state()` function remains declared
  but unimplemented (scaffold only) and does not set `active_backend`.

## References

- PR #109 / ADR-0767 — Phase 4b.8 ABI scoping (full break deferred).
- ADR-0726 — Vulkan backend removal (VMAF_BACKEND_VULKAN reserved but inactive).
- Source: req "Add `vmaf_context_get_backend` … ADDITIVE (not breaking), can land
  now without waiting for the full ABI break."
