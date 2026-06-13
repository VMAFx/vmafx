<!-- markdownlint-disable MD013 MD060 -->
# ADR-0374: Build-time-optional public APIs return `-ENOSYS` when disabled

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris
- **Tags**: `dnn`, `cuda`, `sycl`, `hip`, `vulkan`, `metal`, `mcp`, `build`, `api`, `fork-local`

## Context

The fork exposes several build-time-optional features behind meson boolean
options: `-Denable_dnn`, `-Denable_cuda`, `-Denable_sycl`, `-Denable_hip`,
`-Denable_vulkan`, `-Denable_metal`, `-Denable_mcp`. Each feature has a
public C-API surface declared in a header under `core/include/libvmaf/`.

When a feature is disabled, its public symbols must still be present in the
final shared/static library so that callers compiled against the headers
always link successfully regardless of the build configuration. The symbols
cannot simply be absent — that would break any downstream binary that
references them conditionally or that links against a pre-built libvmaf
where the build configuration is opaque.

A Phase-A audit (2026-05-10) flagged the five `-ENOSYS` return sites in
`core/src/dnn/dnn_api.c` and the one in `core/src/dnn/dnn_attach_api.c`
as "needs clarification: intentional or real gap?" This ADR records the
answer: they are intentional.

## Decision

Every build-time-optional feature in this fork follows the same disabled-build
stub contract:

1. **Always present**: every public symbol declared in the feature's header is
   defined in the TU, unconditionally, so linking always succeeds.
2. **Returns `-ENOSYS`** from every entry point that would require the feature
   to be active (open, run, close, import, attach, start, etc.).
3. **Returns `0`** (false / unavailable) from the availability probe
   (`vmaf_dnn_available`, `vmaf_cuda_available`, `vmaf_hip_available`, etc.).
4. **Returns `NULL`** from entry points whose return type is a pointer (e.g.
   `vmaf_dnn_session_attached_ep`).
5. **Callers must probe availability first**: check the feature's
   `vmaf_X_available()` function at runtime before calling any other entry
   point. `-ENOSYS` is the "not built in" signal, not a programming error.

The contract is explicitly documented in the file-level comment of each stub
TU (see `dnn_api.c` §"disabled-build stub contract" and `dnn_attach_api.c`
§"Disabled-build stub contract (see ADR-0374)").

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Omit the symbols entirely when disabled; use weak symbols or `#ifdef` at call sites | No stub overhead | Breaks callers that don't `#ifdef` every call site; requires the header itself to `#ifdef` away the declarations, making the API conditional — a maintenance burden and a source of subtle include-order bugs | Rejected |
| Return `-ENOTSUP` instead of `-ENOSYS` | Slightly more descriptive on some platforms | `ENOSYS` is the established POSIX convention for "function not implemented" and is already used by every other disabled-build stub in tree (CUDA, HIP, Vulkan, Metal, MCP) | Rejected in favour of consistency |
| Implement a real fallback when DNN is disabled | Nothing to fall back to — ORT is the only inference runtime; CPU ONNX loading without ORT is out of scope | Adds dead code; the correct answer is "don't call this without enabling DNN" | Rejected |

## Consequences

- **Positive**: downstream binaries link against any configuration of libvmaf
  without conditional compilation. Feature probing is uniform across all
  optional backends: `if (!vmaf_X_available()) { /* skip */ }`.
- **Positive**: the audit ambiguity is resolved with a single ADR entry;
  future sessions will not re-investigate these sites.
- **Negative**: callers that forget to probe availability will receive
  `-ENOSYS` at runtime rather than a link error. The mitigation is the
  availability probe API and this documented contract.
- **Neutral**: no code change is required — the stubs were already correct.
  This ADR is documentation-only. The file-level comments in `dnn_api.c`
  and `dnn_attach_api.c` now cross-reference this ADR.

## References

- `core/src/dnn/dnn_api.c` lines 14–17 (existing stub-contract comment)
- `core/src/dnn/dnn_attach_api.c` lines 14–24 (stub-contract comment added in this PR)
- Same pattern in: `core/src/sycl/dmabuf_import.cpp` lines 529–588,
  `core/src/mcp/mcp.c` (scaffold stubs per ADR-0209),
  `core/src/hip/common.c` + `kernel_template.c` (scaffold stubs per ADR-0212),
  `core/src/vulkan/common.c` (scaffold stubs per ADR-0175),
  `core/src/feature/metal/` (scaffold stubs per ADR-0361).
- Phase-A audit finding: `dnn_api.c` and `dnn_attach_api.c` `-ENOSYS` stubs
  flagged as "Phase-A-ish — needs clarification: intentional or real gaps?"
  Decision: intentional per this ADR.
- req: "handle the 5 -ENOSYS stubs in dnn_api.c (and 1 in dnn_attach_api.c)
  that fire when -Denable_dnn=false ... are they intentional or real gaps?"
