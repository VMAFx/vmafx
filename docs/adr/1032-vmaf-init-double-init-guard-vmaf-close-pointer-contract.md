<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1032: vmaf_init double-init guard and vmaf_close pointer-contract documentation

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `api`, `correctness`, `memory-safety`

## Context

Two latent API-contract bugs were identified in `core/src/libvmaf.c` during an
API boundary audit (r6-libvmaf-api-contract), rated HIGH severity:

1. **vmaf_init double-init leak** — `vmaf_init` accepted a non-NULL `*vmaf`
   pointer and unconditionally overwrote it with a fresh allocation, silently
   leaking the old context and its sub-allocations (feature extractor vector,
   feature collector, thread pool, DNN session, picture pool, GPU state).
   The SEI CERT MEM31-C rule requires that memory is always freed before its
   last pointer is overwritten; the double-init path violated this.

2. **vmaf_close leaves dangling pointer** — `vmaf_close(VmafContext *vmaf)`
   frees `vmaf` but cannot zero the caller's handle variable because it only
   receives the value, not the address.  The existing NULL guard at the top of
   `vmaf_close` therefore does not protect against a second call: the callee
   sees a non-NULL but freed pointer, which is undefined behaviour.

A third closely related bug was found in `core/src/dnn/dnn_api.c`:

1. **DNN fp32 fallback does not fall back** — the comment above the
   int8-sidecar validation block stated "fall back to fp32; better degraded
   than dead", but the code returned the validation error code instead.  Any
   model with `quant_mode != FP32` and a missing or invalid `.int8.onnx` file
   failed to load entirely, contrary to the stated intent.

## Decision

**Fix 1 (double-init guard):** Add `if (*vmaf) return -EINVAL;` immediately
after the NULL-pointer check in `vmaf_init`.  Returning `-EINVAL` on a
non-NULL `*vmaf` surfaces the bug at the call site rather than masking it with
a silent leak.

**Fix 2 (close contract):** Document the post-close pointer-invalidity contract
in the `vmaf_close` Doxygen block in `core/include/libvmaf/libvmaf.h`.  Changing
the public signature to `vmaf_close(VmafContext **)` would be an ABI break
incompatible with the fork's ABI-stability commitment; documentation is the
least-invasive safe fix.  Callers that need protection against accidental
reuse must null their pointer after the call, as shown in the new `@code`
example.

**Fix 3 (DNN fallback):** Replace the early `return rc;` in the int8-sidecar
validation failure path with `rc = 0; /* fall through */`, preserving `has_sidecar`
and `meta` so the quant-mode annotation survives, but loading the fp32 baseline
model via `vmaf_ort_open`.  A `VMAF_LOG_LEVEL_DEBUG` log line makes the
degradation observable.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Change `vmaf_close` signature to `VmafContext **` | Zeroes caller pointer automatically | ABI break; breaks every downstream consumer | ABI stability commitment |
| No-op on double `vmaf_init` (silent ignore) | Less disruptive to buggy callers | Masks bugs instead of surfacing them; CERT MEM31-C violation | Correctness over convenience |
| Hard error on DNN fallback (keep original behaviour) | Predictable; forces callers to ship int8 files | Violates the "better degraded than dead" design principle in the comment | The stated intent is fp32 degradation |

## Consequences

- **Positive**: double-init now returns an error immediately, preventing memory
  leaks in misconfigured callers; the `vmaf_close` contract is explicit in the
  API header; DNN sessions degrade gracefully when only fp32 weights are
  present.
- **Negative**: callers that accidentally passed a non-NULL `*vmaf` to
  `vmaf_init` will now get `-EINVAL` instead of silently succeeding (and
  leaking) — this is the desired behaviour but is technically a behavioural
  change.
- **Neutral**: a unit test (`test_vmaf_init_double_init_guard` in
  `core/test/test_context.c`) exercises the guard in the `fast` suite.

## References

- req: findings from r6-libvmaf-api-contract + r6-dnn-onnx-boundary HIGH-severity review
- SEI CERT MEM31-C: free dynamically allocated memory when no longer needed
- [ADR-0108](0108-deep-dive-deliverables-rule.md): deep-dive deliverables rule
